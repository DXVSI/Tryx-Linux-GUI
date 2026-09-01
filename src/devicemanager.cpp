#include "devicemanager.h"
#include "deleteintentstore.h"
#include "devicemediaartifactstore.h"
#include "devicemanagermessages.h"
#include "mediacatalogstore.h"
#include "mediatransform.h"
#include "paseoverlayconfig.h"
#include "pasemetricsconfigstore.h"
#include "privateruntimepaths.h"
#include "printermediafileintegrity.h"
#include "printermediaidentity.h"
#include "printermediapreparer.h"
#include "printermediavalidator.h"
#include "printerprotocol.h"
#include "runtimeapplyrequestcodec.h"
#include "runtimedowngradestore.h"
#include "runtimepresentationpreferencesstore.h"
#include "savedlayoutstore.h"
#include "supportsnapshot.h"
#include "systemmonitor.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <utility>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// --- DeviceWorker ---

namespace {

using tryx::printer_media_file_integrity::isSha256Hex;
using tryx::printer_media_file_integrity::sha256File;
using tryx::printer_media_file_integrity::sourceFingerprint;
using tryx::private_runtime_paths::atomicRenameNoReplace;
using tryx::private_runtime_paths::cleanAbsolutePath;
using tryx::private_runtime_paths::ensurePrivateDirectory;
using tryx::private_runtime_paths::pathIsInside;
using tryx::private_runtime_paths::stagedSourceFileNameIsValid;
using tryx::private_runtime_paths::stagedSourceStatIsValid;
using tryx::printer_media_identity::generatedPrinterMediaName;
using tryx::printer_media_identity::h264PrinterNameForConversion;
using tryx::printer_media_identity::printerConversionProfile;
using tryx::printer_media_identity::printerConversionProfileMatchesProduct;
using tryx::printer_media_identity::paseRecoveredConversionProfile;
using tryx::printer_media_identity::printerMediaConversionIdentity;
using tryx::printer_media_identity::printerMediaNameMatchesProfile;
using tryx::printer_media_validator::RecoveredH264ProbeMetadata;
using tryx::printer_media_validator::validateRecoveredH264;
using tryx::pase_overlay_config::hasDuplicateMetricLabels;
using tryx::pase_overlay_config::hasDuplicateValues;
using tryx::pase_overlay_config::isSupportedPaseBadge;
using tryx::pase_overlay_config::isSupportedPaseMetricLabel;
using tryx::pase_overlay_config::isValidPaseTextColor;
using tryx::pase_overlay_config::normalizeAndValidatePaseApplyOverlayStyles;
using tryx::pase_overlay_config::paseOverlayFromApplyRequest;
using tryx::pase_overlay_config::paseOverlayFromMetricsRequest;
using tryx::pase_overlay_config::paseOverlayHasContent;
using tryx::pase_overlay_config::paseOverlayHasMetrics;
using tryx::pase_overlay_config::paseOverlayRequestsBadge;
using tryx::pase_overlay_config::paseTextColorName;
using tryx::pase_overlay_config::paseUploadApplyRequestIsValid;
using tryx::pase_overlay_config::printerMediaConfigName;
using tryx::pase_overlay_config::printerPresetMediaFile;
using tryx::runtime_apply_request_codec::runtimeApplyRequestFingerprint;
using tryx::runtime_apply_request_codec::runtimeApplyRequestFromJson;
using tryx::runtime_apply_request_codec::runtimeApplyRequestToJson;
using tryx::runtime_apply_request_codec::runtimeMediaTransformRequestFingerprint;

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

QString savedLayoutStoreErrorName(
    tryx::SavedLayoutStore::ErrorCode code) {
    using ErrorCode = tryx::SavedLayoutStore::ErrorCode;
    switch (code) {
    case ErrorCode::None:
        return {};
    case ErrorCode::RevisionConflict:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict");
    case ErrorCode::NameConflict:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutNameConflict");
    case ErrorCode::CommitUnknown:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsCommitUnknown");
    case ErrorCode::InvalidInput:
    case ErrorCode::ResourceLimitExceeded:
    case ErrorCode::NotFound:
        return QStringLiteral(
            "org.tryx.Panorama.Error.InvalidSavedLayout");
    case ErrorCode::WritesDisabled:
    case ErrorCode::DirectoryUnavailable:
    case ErrorCode::UnsafePath:
    case ErrorCode::WriteFailed:
    case ErrorCode::CommitFailed:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsUnavailable");
    }
    return QStringLiteral(
        "org.tryx.Panorama.Error.SavedLayoutsUnavailable");
}

constexpr int kMaxPrinterKeepaliveWriteRetries = 3;
constexpr int kPrinterKeepaliveRetryBackoffMs = 500;
constexpr int kMaxTerminalOperationHistory = 32;
constexpr qint64 kMaxRetryCacheBytes =
    tryx::printer_media_file_integrity::kMaximumPreparedMediaBytes;
constexpr qint64 kMaxThumbnailBytes =
    tryx::printer_media_file_integrity::kMaximumThumbnailBytes;
constexpr auto kRetryCacheTransitionConflictId =
    "retry-cache-transition-conflict";
constexpr qint64 kFileTransmitChunkSize = 0x40000;
constexpr qint64 kMediaInboxMaxAgeSeconds = 24LL * 60LL * 60LL;
constexpr int kDeviceMediaSweepIntervalMs = 5000;
constexpr qint64 kRecoveredMediaFreeSpaceReserveBytes =
    16LL * 1024LL * 1024LL;
constexpr quint16 kTurrisProductId = 0x2011;

bool filesystemLeafExistsOrIsAmbiguous(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    errno = 0;
    if (::lstat(encoded.constData(), &status) == 0) {
        return true;
    }
    return errno != ENOENT;
}

tryx::MediaCatalogStore::RemoteEntry mediaCatalogRemoteEntry(
    const TryxRuntimeMediaEntry &entry) {
    tryx::MediaCatalogStore::RemoteEntry remote;
    remote.name = entry.name;
    remote.size = entry.size;
    remote.source = entry.source;
    remote.readOnly = entry.readOnly;
    return remote;
}

tryx::MediaCatalogStore::RemoteEntry mediaCatalogRemoteEntry(
    const PrinterProtocol::MediaFile &media) {
    tryx::MediaCatalogStore::RemoteEntry remote;
    remote.name = media.name;
    remote.size = media.size;
    remote.source = media.source == PrinterProtocol::MediaSource::Preset
        ? 2U
        : 1U;
    remote.readOnly = media.readOnly;
    return remote;
}

bool retryCacheDispatchRetiredIntoCleanup(
    const tryx::RetryCacheStore::Snapshot &snapshot) {
    return !snapshot.cleanupPending.isEmpty() &&
        !snapshot.inFlightDispatch.has_value();
}

struct PrinterProcessClock {
    PrinterProcessClock() {
        timer.start();
    }

    QElapsedTimer timer;
};

qint64 printerMonotonicMilliseconds() {
    static const PrinterProcessClock clock;
    return clock.timer.elapsed();
}

QString printerStructuredValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString printerOverlayLeaseModeName(PrinterOverlayLeaseMode mode) {
    switch (mode) {
    case PrinterOverlayLeaseMode::PingAndOverlayLease:
        return QStringLiteral("ping-and-overlay-lease");
    case PrinterOverlayLeaseMode::PingOnly:
        return QStringLiteral("ping-only");
    }
    return QStringLiteral("unknown");
}

QString deviceMediaArtifactErrorText(
    tryx::DeviceMediaArtifactStore::ErrorCode code,
    const QString &detail = {}) {
    switch (code) {
    case tryx::DeviceMediaArtifactStore::ErrorCode::None:
        return {};
    case tryx::DeviceMediaArtifactStore::ErrorCode::InvalidOwner:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact belongs to another caller");
    case tryx::DeviceMediaArtifactStore::ErrorCode::InvalidLease:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact lease is invalid");
    case tryx::DeviceMediaArtifactStore::ErrorCode::Expired:
    case tryx::DeviceMediaArtifactStore::ErrorCode::Revoked:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact lease has expired");
    case tryx::DeviceMediaArtifactStore::ErrorCode::UnsafePath:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact escaped its private outbox");
    case tryx::DeviceMediaArtifactStore::ErrorCode::IdentityChanged:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact identity changed");
    case tryx::DeviceMediaArtifactStore::ErrorCode::HashChanged:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact hash changed");
    case tryx::DeviceMediaArtifactStore::ErrorCode::NotClaimed:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact has not been claimed");
    case tryx::DeviceMediaArtifactStore::ErrorCode::Busy:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact is held by an active operation");
    case tryx::DeviceMediaArtifactStore::ErrorCode::NotFound:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact does not exist");
    default:
        return detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("The device media artifact is invalid")
            : detail;
    }
}

TryxRuntimeDeviceMediaArtifact runtimeDeviceMediaArtifact(
    const tryx::DeviceMediaArtifactStore::ClaimResult &claim) {
    TryxRuntimeDeviceMediaArtifact artifact;
    artifact.schemaVersion = claim.metadata.schemaVersion;
    artifact.operationId = claim.metadata.operationId;
    artifact.artifactId = claim.metadata.artifactId;
    artifact.mediaId = claim.metadata.mediaId;
    artifact.deviceIdentity = claim.metadata.deviceIdentity;
    artifact.remoteName = claim.metadata.remoteName;
    artifact.size = claim.metadata.size;
    artifact.decodedSha256 = claim.metadata.decodedSha256;
    artifact.localPath = claim.localPath;
    artifact.logicalType = claim.metadata.logicalType;
    artifact.leaseId = claim.leaseId;
    artifact.leaseExpiresUtcMs = claim.leaseExpiresUtcMs;
    return artifact;
}

TryxRuntimeDeviceMediaMetadataV1 runtimeDeviceMediaMetadata(
    const tryx::DeviceMediaArtifactStore::Metadata &stored) {
    TryxRuntimeDeviceMediaMetadataV1 metadata;
    metadata.schemaVersion = stored.schemaVersion;
    metadata.operationId = stored.operationId;
    metadata.artifactId = stored.artifactId;
    metadata.mediaId = stored.mediaId;
    metadata.deviceIdentity = stored.deviceIdentity;
    metadata.decodedSha256 = stored.decodedSha256;
    metadata.deviceGeneration = stored.deviceGeneration;
    metadata.status = stored.status;
    metadata.availableFields = stored.availableFields;
    metadata.width = stored.width;
    metadata.height = stored.height;
    metadata.durationMilliseconds = stored.durationMilliseconds;
    metadata.frameRateNumerator = stored.frameRateNumerator;
    metadata.frameRateDenominator = stored.frameRateDenominator;
    return metadata;
}

QString printerKeepaliveOutcomeName(
    PrinterProtocol::KeepaliveOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::KeepaliveOutcome::Sent:
        return QStringLiteral("sent");
    case PrinterProtocol::KeepaliveOutcome::RetryableFailure:
        return QStringLiteral("retryable-failure");
    case PrinterProtocol::KeepaliveOutcome::FatalFailure:
        return QStringLiteral("fatal-failure");
    }
    return QStringLiteral("unknown");
}

QString printerDiscoveryStateName(
    PrinterProtocol::DiscoveryState state) {
    switch (state) {
    case PrinterProtocol::DiscoveryState::Absent:
        return QStringLiteral("absent");
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
        return QStringLiteral("rockchip-gadget-391a-0006");
    case PrinterProtocol::DiscoveryState::EnumeratingPrinterClass:
        return QStringLiteral("enumerating-printer-class");
    case PrinterProtocol::DiscoveryState::Ready:
        return QStringLiteral("ready");
    case PrinterProtocol::DiscoveryState::PermissionDenied:
        return QStringLiteral("permission-denied");
    case PrinterProtocol::DiscoveryState::Ambiguous:
        return QStringLiteral("ambiguous");
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        return QStringLiteral("monitoring-unavailable");
    }
    return QStringLiteral("unknown");
}

void logPrinterLifecycleEvent(
    const QString &eventName, quint64 generation,
    std::initializer_list<QPair<QString, QString>> fields = {}) {
    QStringList parts{
        QStringLiteral("tryx_lifecycle"),
        QStringLiteral("event=") + printerStructuredValue(eventName),
        QStringLiteral("utc=") +
            printerStructuredValue(
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),
        QStringLiteral("monotonic_ms=") +
            QString::number(printerMonotonicMilliseconds()),
        QStringLiteral("generation=") + QString::number(generation)
    };
    QList<QPair<QString, QString>> supportFields;
    supportFields.reserve(static_cast<qsizetype>(fields.size()));
    for (const auto &field : fields) {
        parts.append(field.first + QLatin1Char('=') +
                     printerStructuredValue(field.second));
        supportFields.append(field);
    }
    tryx::appendSupportLifecycleEvent(
        eventName, generation, supportFields);
    qInfo().noquote() << parts.join(QLatin1Char(' '));
}

QString mutationOutcomeName(PrinterProtocol::MutationOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::MutationOutcome::NotStarted:
        return QStringLiteral("NotStarted");
    case PrinterProtocol::MutationOutcome::Rejected:
        return QStringLiteral("Rejected");
    case PrinterProtocol::MutationOutcome::VerificationFailed:
        return QStringLiteral("VerificationFailed");
    case PrinterProtocol::MutationOutcome::Succeeded:
        return QStringLiteral("Succeeded");
    case PrinterProtocol::MutationOutcome::Cancelled:
        return QStringLiteral("Cancelled");
    case PrinterProtocol::MutationOutcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    case PrinterProtocol::MutationOutcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    }
    return QStringLiteral("PartialOrUnknown");
}

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

const GpuMetrics *gpuForPin(const SystemMetrics &metrics,
                            tryx::GpuSelectionPin *pin) {
    const qsizetype index = tryx::resolveGpuSelectionPin(
        metrics.gpus, pin);
    return index >= 0 ? &metrics.gpus.at(index) : nullptr;
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

void collectPaseMetricValues(const SystemMetrics &metrics,
                             const GpuMetrics *selectedGpu,
                             const QString &temperatureUnit,
                             QStringList *labels,
                             QStringList *values, QStringList *units) {
    labels->clear();
    values->clear();
    units->clear();
    const auto appendMetric = [labels, values, units](
                                  const QString &label, double value,
                                  const QString &unit, bool available,
                                  int precision = 0) {
        if (!available) {
            return;
        }
        labels->append(label);
        values->append(QString::number(value, 'f', precision));
        units->append(unit);
    };
    const QString temperatureSymbol =
        tryxTemperatureUnitSymbol(temperatureUnit);
    const auto appendTemperature =
        [labels, values, units, &temperatureUnit,
         &temperatureSymbol](const QString &label, double celsius,
                             bool available) {
            if (!available) {
                return;
            }
            const QString value = tryxFormatTemperatureValue(
                celsius, temperatureUnit);
            if (value.isEmpty() || temperatureSymbol.isEmpty()) {
                return;
            }
            labels->append(label);
            values->append(value);
            units->append(temperatureSymbol);
        };
    appendTemperature(QStringLiteral("CPU Temperature"),
                      metrics.cpu.temperature,
                      metrics.cpu.temperatureAvailable);
    appendMetric(QStringLiteral("CPU Frequency"), metrics.cpu.frequencyMHz,
                 QStringLiteral("MHZ"), metrics.cpu.frequencyAvailable);
    appendMetric(QStringLiteral("CPU Usage"), metrics.cpu.usagePercent,
                 QStringLiteral("%"), metrics.cpu.usageAvailable);
    appendMetric(QStringLiteral("CPU Power"), metrics.cpu.powerWatts,
                 QStringLiteral("W"), metrics.cpu.powerAvailable, 1);
    if (selectedGpu) {
        const GpuMetrics &gpu = *selectedGpu;
        appendTemperature(QStringLiteral("GPU Temperature"),
                          gpu.temperature,
                          gpu.temperatureAvailable);
        appendMetric(QStringLiteral("GPU Frequency"), gpu.frequencyMHz,
                     QStringLiteral("MHZ"), gpu.frequencyAvailable);
        appendMetric(QStringLiteral("GPU Usage"), gpu.usagePercent,
                     QStringLiteral("%"), gpu.usageAvailable);
        appendMetric(QStringLiteral("GPU Power"), gpu.powerWatts,
                     QStringLiteral("W"), gpu.powerAvailable, 1);
    }
    appendMetric(QStringLiteral("Memory Frequency"),
                 metrics.ram.frequencyMHz, QStringLiteral("MHZ"),
                 metrics.ram.frequencyAvailable);
    appendMetric(QStringLiteral("Memory Usage"), metrics.ram.usagePercent,
                 QStringLiteral("%"), metrics.ram.usageAvailable);
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

QString retryCacheTerminalOutcomeName(
    tryx::RetryCacheStore::TerminalOutcome outcome) {
    switch (outcome) {
    case tryx::RetryCacheStore::TerminalOutcome::NotStarted:
        return QStringLiteral("NotStarted");
    case tryx::RetryCacheStore::TerminalOutcome::Rejected:
        return QStringLiteral("Rejected");
    case tryx::RetryCacheStore::TerminalOutcome::Cancelled:
        return QStringLiteral("Cancelled");
    case tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    case tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    }
    return QStringLiteral("PartialOrUnknown");
}

bool retryCacheDispatchPhaseIsRestricted(
    tryx::RetryCacheStore::DispatchPhase phase) {
    return phase ==
               tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence ||
        phase == tryx::RetryCacheStore::DispatchPhase::
                     ShadowMissingFenceReconnectPending;
}

}  // namespace

// --- DeviceWorker ---

DeviceWorker::DeviceWorker(QObject *parent)
    : QObject(parent),
      printerProtocol_(std::make_unique<PrinterProtocol>()),
      legacyMetricsTimer_(new QTimer(this)),
      printerKeepaliveTimer_(new QTimer(this)),
      printerMetricsTimer_(new QTimer(this)),
      printerRecoveryTimer_(new QTimer(this)),
      printerSystemMonitor_(new SystemMonitor(this)),
      printerCancellationFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      printerOperationCancellationFd_(
          eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    legacyMetricsTimer_->setInterval(1000);
    connect(legacyMetricsTimer_, &QTimer::timeout,
            this, &DeviceWorker::sendLegacyMetrics);
    printerKeepaliveTimer_->setSingleShot(true);
    printerKeepaliveTimer_->setInterval(2000);
    connect(printerKeepaliveTimer_, &QTimer::timeout,
            this, &DeviceWorker::sendPrinterKeepalive);
    printerMetricsTimer_->setInterval(1000);
    connect(printerMetricsTimer_, &QTimer::timeout,
            this, &DeviceWorker::sendPrinterMetrics);
    printerRecoveryTimer_->setSingleShot(true);
    connect(printerRecoveryTimer_, &QTimer::timeout,
            this, &DeviceWorker::retryPrinterSessionStart);
    connect(printerSystemMonitor_, &SystemMonitor::metricsUpdated,
            this, &DeviceWorker::publishPrinterMetricsAvailability);
}

DeviceWorker::~DeviceWorker() {
    stopPrinterSession();
    if (printerCancellationFd_ >= 0) {
        ::close(printerCancellationFd_);
        printerCancellationFd_ = -1;
    }
    if (printerOperationCancellationFd_ >= 0) {
        ::close(printerOperationCancellationFd_);
        printerOperationCancellationFd_ = -1;
    }
    if (device_ && device_->is_connected()) {
        device_->disconnect();
    }
}

void DeviceWorker::updatePrinterGenerationGate(quint64 generation,
                                                bool endpointReady) {
    printerGenerationGate_.store(generation, std::memory_order_release);
    printerEndpointReady_.store(endpointReady, std::memory_order_release);
    if (printerCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored = ::write(printerCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
    if (printerOperationCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored =
            ::write(printerOperationCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
}

void DeviceWorker::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerOverlayLeaseMode_ = mode;
    if (mode == PrinterOverlayLeaseMode::PingOnly) {
        printerOverlayLeaseRefreshNext_ = false;
    }
}

void DeviceWorker::publishPresentationPreferences(
    const TryxRuntimePresentationPreferencesV1 &preferences) {
    if (!tryxPresentationPreferencesAreValid(preferences)) {
        return;
    }
    unsigned int encoded = 0;
    if (preferences.temperatureUnit == QStringLiteral("Fahrenheit")) {
        encoded |= 1U;
    }
    if (preferences.timeFormat == QStringLiteral("12H")) {
        encoded |= 2U;
    }
    publishedPresentationPreferences_.store(
        encoded, std::memory_order_release);
}

void DeviceWorker::applyPublishedPresentationPreferences() {
    synchronizePublishedPresentationPreferences();
}

void DeviceWorker::synchronizePublishedPresentationPreferences() {
    const unsigned int encoded =
        publishedPresentationPreferences_.load(
            std::memory_order_acquire);
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

void DeviceWorker::cancelPrinterOperation(const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&printerOperationCancellationMutex_);
        cancelledPrinterOperationIds_.insert(operationId);
    }
    if (printerOperationCancellationFd_ < 0) {
        return;
    }
    const uint64_t value = 1;
    const ssize_t ignored =
        ::write(printerOperationCancellationFd_, &value, sizeof(value));
    Q_UNUSED(ignored);
}

void DeviceWorker::clearPrinterOperationCancellation(
    const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&printerOperationCancellationMutex_);
    cancelledPrinterOperationIds_.remove(operationId);
}

#ifdef TRYX_PROTOCOL_TESTING
void DeviceWorker::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    stopPrinterSession();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerProtocol_->adoptFileDescriptorForTesting(fd, devicePath);
}

bool DeviceWorker::printerSessionActiveForTesting() const {
    return printerSessionState_ == PrinterSessionState::Active;
}
#endif

void DeviceWorker::connectDevice(const QString &port) {
    std::string portStr;

    if (port.isEmpty()) {
        auto detected = panorama::Device::find_device();
        if (!detected) {
            emit error(tr("Device not found. Check the USB connection."));
            return;
        }
        portStr = *detected;
    } else {
        portStr = port.toStdString();
    }

    device_ = std::make_unique<panorama::Device>(portStr);
    if (!device_->connect()) {
        emit error(tr("Failed to connect to %1").arg(QString::fromStdString(portStr)));
        device_.reset();
        return;
    }

    doHandshake();
}

void DeviceWorker::disconnectDevice() {
    legacyMetricsTimer_->stop();
    if (!device_) {
        return;
    }
    device_->disconnect();
    device_.reset();
    emit disconnected();
}

void DeviceWorker::doHandshake() {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto info = device_->handshake();
    if (!info) {
        emit error(tr("Handshake failed"));
        return;
    }

    emit connected(
        QString::fromStdString(info->product_id),
        QString::fromStdString(info->serial),
        QString::fromStdString(info->firmware),
        QString::fromStdString(info->app_version)
    );
    legacyMetricsTimer_->start();
    QTimer::singleShot(
        0, this, &DeviceWorker::sendLegacyMetrics);
}

void DeviceWorker::setBrightness(int value) {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto resp = device_->set_brightness(value);
    if (!resp) {
        emit error(tr("Failed to set brightness"));
        return;
    }
    emit brightnessSet(value);
}

void DeviceWorker::setScreenConfig(const QStringList &media, const QString &ratio,
                                   const QString &screenMode, const QString &playMode,
                                   const QStringList &sysinfoLabels,
                                   const QString &settingsPosition,
                                   const QString &settingsColor,
                                   const QString &settingsAlign,
                                   const QStringList &settingsBadges,
                                   int filterOpacity,
                                   const QString &presetId,
                                   const QStringList &sysinfoLabels2,
                                   const QStringList &settingsBadges2,
                                   bool waterfallMode) {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    panorama::ScreenConfig config;
    if (!presetId.isEmpty()) {
        config.preset_id = presetId.toStdString();
    }
    for (const auto &m : media) {
        config.media.push_back(m.toStdString());
    }
    config.ratio = ratio.toStdString();
    config.screen_mode = screenMode.toStdString();
    config.play_mode = playMode.toStdString();

    for (const auto &label : sysinfoLabels) {
        config.sysinfo_display.push_back(label.toStdString());
    }

    config.settings.position = settingsPosition.toStdString();
    config.settings.color = settingsColor.toStdString();
    config.settings.align = settingsAlign.toStdString();
    config.settings.filter_opacity = filterOpacity;
    for (const auto &badge : settingsBadges) {
        config.settings.badges.push_back(badge.toStdString());
    }

    config.waterfall_mode = waterfallMode;

    // Screen Splitting: populate second set of settings and sysinfo
    if (screenMode == "Screen Splitting") {
        for (const auto &label : sysinfoLabels2) {
            config.sysinfo_display2.push_back(label.toStdString());
        }
        config.settings2.position = settingsPosition.toStdString();
        config.settings2.color = settingsColor.toStdString();
        config.settings2.align = settingsAlign.toStdString();
        config.settings2.filter_opacity = filterOpacity;
        for (const auto &badge : settingsBadges2) {
            config.settings2.badges.push_back(badge.toStdString());
        }
    }

    auto resp = device_->set_screen_config(config);
    if (!resp) {
        emit error(tr("Failed to set display configuration"));
        return;
    }

    // Send sysinfoDisplay as separate command if metrics are selected
    if (!config.sysinfo_display.empty()) {
        device_->set_sysinfo_display(config);
    }

    // Send config with hardware names for badges
    std::string cpuName = "Unknown CPU";
    std::string gpuName = "Unknown GPU";

    // Read CPU name from /proc/cpuinfo
    {
        std::ifstream cpuFile("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuFile, line)) {
            if (line.find("model name") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos && pos + 2 < line.size()) {
                    cpuName = line.substr(pos + 2);
                }
                break;
            }
        }
    }

    // Read GPU name: try sysfs product_name first, fallback to lspci
    {
        namespace fs = std::filesystem;
        std::string drmPath = "/sys/class/drm";
        if (fs::exists(drmPath)) {
            for (const auto& entry : fs::directory_iterator(drmPath)) {
                std::string name = entry.path().filename().string();
                if (name.find("card") == 0 && name.find('-') == std::string::npos) {
                    std::string productPath = entry.path().string() + "/device/product_name";
                    std::ifstream gpuFile(productPath);
                    if (gpuFile) {
                        std::string readName;
                        std::getline(gpuFile, readName);
                        if (!readName.empty()) {
                            gpuName = readName;
                            break;
                        }
                    }
                }
            }
        }
        // Fallback 1: glxinfo gives clean name like "AMD Radeon RX 7900 XTX"
        if (gpuName == "Unknown GPU") {
            FILE* pipe = popen("glxinfo 2>/dev/null | grep 'OpenGL renderer' | head -1", "r");
            if (pipe) {
                char buf[512];
                if (fgets(buf, sizeof(buf), pipe)) {
                    std::string line(buf);
                    auto pos = line.find(": ");
                    if (pos != std::string::npos) {
                        gpuName = line.substr(pos + 2);
                        // Cut at first '(' - remove "(radeonsi, navi31, ...)"
                        auto paren = gpuName.find('(');
                        if (paren != std::string::npos)
                            gpuName = gpuName.substr(0, paren);
                        while (!gpuName.empty() && (gpuName.back() == '\n' || gpuName.back() == '\r' || gpuName.back() == ' '))
                            gpuName.pop_back();
                    }
                }
                pclose(pipe);
            }
        }
        // Fallback 2: lspci
        if (gpuName == "Unknown GPU" || gpuName.empty()) {
            FILE* pipe = popen("lspci 2>/dev/null | grep -i 'VGA\\|3D controller' | head -1", "r");
            if (pipe) {
                char buf[512];
                if (fgets(buf, sizeof(buf), pipe)) {
                    std::string line(buf);
                    auto pos = line.find(": ");
                    if (pos != std::string::npos) {
                        gpuName = line.substr(pos + 2);
                        while (!gpuName.empty() && (gpuName.back() == '\n' || gpuName.back() == '\r'))
                            gpuName.pop_back();
                    }
                }
                pclose(pipe);
            }
        }
    }

    fprintf(stderr, "[config] cpu='%s' gpu='%s'\n", cpuName.c_str(), gpuName.c_str());

    // Send full config (KANALI format) - sets everything in one command
    device_->send_full_config(config, cpuName, gpuName, 75, "Celsius");

    emit screenConfigSet();
}

void DeviceWorker::sendSysinfo(const QStringList &labels, const QStringList &values,
                               const QStringList &units) {
    if (!device_ || !device_->is_connected()) {
        return;
    }

    std::vector<panorama::SysinfoData> data;
    for (int i = 0; i < labels.size() && i < values.size() && i < units.size(); ++i) {
        panorama::SysinfoData item;
        item.label = labels[i].toStdString();
        item.value = values[i].toStdString();
        item.unit = units[i].toStdString();
        data.push_back(item);
    }

    device_->send_sysinfo(data);
    emit sysinfoSent();
}

void DeviceWorker::sendLegacyMetrics() {
    if (!device_ || !device_->is_connected()) {
        legacyMetricsTimer_->stop();
        printerSystemMonitor_->setNvidiaSampleDemand(
            tryx::nvidia::NvidiaSampleDemand::Off);
        return;
    }

    printerSystemMonitor_->setNvidiaSampleDemand(
        tryx::nvidia::NvidiaSampleDemand::Active);
    printerSystemMonitor_->update();
    const SystemMetrics metrics =
        printerSystemMonitor_->currentMetrics();
    tryx::GpuSelectionPin primaryPin =
        tryx::primaryGpuSelectionPin(metrics.gpus);
    QStringList labels;
    QStringList values;
    QStringList units;
    collectPaseMetricValues(
        metrics, gpuForPin(metrics, &primaryPin),
        tryxDefaultTemperatureUnit(),
        &labels, &values, &units);
    if (!labels.isEmpty()) {
        sendSysinfo(labels, values, units);
    }
}

void DeviceWorker::deleteMedia(const QStringList &files) {
    std::vector<std::string> filenames;
    for (const auto &f : files) {
        filenames.push_back(f.toStdString());
    }

    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto resp = device_->delete_media(filenames);
    if (!resp) {
        emit error(tr("Failed to delete media files"));
        return;
    }

    for (const auto &f : files) {
        panorama::Adb::remove(f.toStdString());
    }

    emit mediaDeleted();
}

void DeviceWorker::uploadMedia(const QString &localPath) {
    if (!panorama::Adb::is_device_connected()) {
        emit error(tr("ADB device not found"));
        return;
    }

    std::string path = localPath.toStdString();
    auto type = panorama::Media::detect_type(path);

    std::string remoteName;
    std::string uploadPath = path;

    if (panorama::Media::needs_conversion(path)) {
        remoteName = panorama::Media::get_converted_name(path);
    } else {
        remoteName = panorama::Media::get_filename(path);
    }

    // Check if file already exists on device - skip upload
    if (panorama::Adb::file_exists(remoteName)) {
        emit mediaUploaded(QString::fromStdString(remoteName));
        return;
    }

    // Need to upload - convert if necessary
    if (panorama::Media::needs_conversion(path)) {
        if (!panorama::Media::is_ffmpeg_available()) {
            emit error(tr("ffmpeg not found. Install it with your system package manager"));
            return;
        }
        emit uploadProgress(tr("Converting to MP4..."));
        std::string converted = std::string(panorama::Media::TMP_DIR) + remoteName;
        bool ok = (type == panorama::MediaType::Gif)
            ? panorama::Media::convert_gif_to_mp4(path, converted)
            : panorama::Media::convert_to_mp4(path, converted);
        if (!ok) {
            emit error(tr("Conversion to MP4 failed"));
            return;
        }
        uploadPath = converted;
    }

    emit uploadProgress(tr("Uploading to device..."));
    if (!panorama::Adb::push(uploadPath, remoteName)) {
        emit error(tr("Upload to device failed"));
        return;
    }

    emit mediaUploaded(QString::fromStdString(remoteName));
}

void DeviceWorker::uploadPreparedPrinterMedia(const QString &devicePath,
                                              const QString &uploadPath,
                                              const QString &remoteName,
                                              const QString &expectedSha256,
                                              const QString &operationId,
                                              quint64 generation) {
    if (!printerGenerationIsCurrent(generation)) {
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::Cancelled,
            tr("Printer-class upload was cancelled because the USB device changed"),
            generation);
        return;
    }
    const QFileInfo preparedInfo(uploadPath);
    if (!preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() || preparedInfo.size() <= 0 ||
        !isSha256Hex(expectedSha256)) {
        emit printerPreparedFileConsumed(uploadPath);
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            tr("Prepared printer-class media is not available"), generation);
        return;
    }

    QString uploadedName;
    QString errorMessage;
    PrinterProtocol::OperationContext context;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive =
        printerProtocol_->productProfile().idleMode ==
        PrinterIdleMode::OverlayLayout;

    emit printerForegroundProgress(
        operationId, QStringLiteral("Beginning"), 0, preparedInfo.size(),
        tr("Starting printer-class upload..."), generation);
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
            emit printerUploadProgress(
                tr("Uploading to printer-class firmware... %1%")
                    .arg(qBound(0, percent, 100)),
                generation);
            emit printerForegroundProgress(
                operationId, QStringLiteral("Transferring"), bytesSent,
                totalBytes,
                tr("Uploading to printer-class firmware... %1%")
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
                    ? tr("The PASE upload outcome is partial or unknown; physically reconnect the device before continuing")
                    : tr("The PASE upload outcome is partial or unknown: %1. Physically reconnect the device before continuing")
                          .arg(errorMessage),
                generation);
        } else {
            schedulePrinterSessionRecovery(errorMessage, generation);
        }
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            mutationDetails.outcome,
            tr("Printer-class upload failed: %1").arg(errorMessage),
            generation);
        return;
    }

    restartPrinterKeepaliveAfterActivity();
    emit printerUploadFinished(
        operationId, uploadPath, uploadedName, true,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation);
}

void DeviceWorker::refreshMediaList() {
    if (!panorama::Adb::is_device_connected()) {
        emit error(tr("ADB device not found"));
        return;
    }

    auto files = panorama::Adb::list_media();
    if (!files) {
        emit error(tr("Failed to retrieve file list"));
        return;
    }

    QStringList list;
    for (const auto &f : *files) {
        if (!f.empty()) {
            list.append(QString::fromStdString(f));
        }
    }
    emit mediaListReady(list);
}


void DeviceWorker::configurePrinterDevice(const QString &devicePath,
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
        emit printerOperationError(
            tr("Unsupported TRYX USB product %1")
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

void DeviceWorker::restorePrinterOverlay(
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
        emit printerSessionStopped(generation);
        restartPrinterKeepaliveAfterActivity();
        return;
    }
    if (printerSessionState_ == PrinterSessionState::Active) {
        startPrinterMetrics();
    }
}

void DeviceWorker::beginPrinterForegroundOperation(
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

void DeviceWorker::endPrinterForegroundOperation(
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

void DeviceWorker::clearPrinterDevice(quint64 generation) {
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

void DeviceWorker::quiesceDeviceTransports(quint64 generation) {
    legacyMetricsTimer_->stop();
    if (device_) {
        if (device_->is_connected()) {
            device_->disconnect();
        }
        device_.reset();
    }

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
        printerGenerationGate_.load(std::memory_order_acquire));
    printerSessionElapsedTimer_.invalidate();
    drainAllPrinterCancellations();
}

void DeviceWorker::quiesceForRuntimeDowngrade(
    quint64 generation) {
    // DeviceManager invokes this with a blocking queued call after publishing
    // its mutation latch and closing the generation gate. Reaching this slot
    // proves that every earlier worker command returned and that worker-owned
    // transport timers cannot write after the durable marker is committed.
    quiesceDeviceTransports(generation);
}

void DeviceWorker::quiesceForFirmware(const QString &leaseId,
                                      quint64 generation) {
    // This slot is deliberately queued on the same worker thread as every
    // device command. Reaching it proves that all commands accepted before the
    // firmware gate have returned. The generation gate is closed synchronously
    // by DeviceManager before this slot is queued, so in-flight printer-class
    // transactions are interrupted and no later transaction can start.
    quiesceDeviceTransports(generation);

    emit firmwareTransportQuiesced(leaseId, generation);
}

void DeviceWorker::releaseFirmwareQuiesceFence(
    const QString &leaseId, quint64 generation) {
    // This no-op fence shares the device worker queue with quiesce and every
    // transport command. Its ACK proves that a previously queued quiesce can
    // no longer run after DeviceManager resumes the transport.
    emit firmwareQuiesceReleaseFenceReached(
        leaseId, generation);
}

void DeviceWorker::readPrinterDeviceInfo(const QString &devicePath,
                                         quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::Result result =
        printerProtocol_->readDeviceInfo(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerDeviceInfoFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerDeviceInfoReady(result.deviceInfo, generation);
}

void DeviceWorker::readPrinterDisplayState(const QString &devicePath,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::PaseDisplayStateResult result =
        printerProtocol_->readPaseDisplayState(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerDisplayStateFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerDisplayStateReady(result.state, generation);
}

void DeviceWorker::refreshPrinterMediaList(const QString &devicePath,
                                           const QString &operationId,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::MediaListResult result =
        printerProtocol_->readMediaList(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerMediaListFailed(
            operationId,
            tr("Failed to read printer-class media list: %1").arg(result.error),
            generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerMediaListReady(operationId, result.files, generation);
}

void DeviceWorker::stagePrinterMedia(
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
            emit printerMediaStaged(
                operationId, mediaName, outputPath, success, cancelled,
                fileSize, chunkCount, rawSha256, decodedSha256,
                probeMetadata, errorMessage, generation);
        };

    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!expectedMediaSize.isValid()) {
        finish(false, false, 0, 0, {}, {},
               tr("Selected media geometry is not supported by this device"));
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
            errorMessage = tr(
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
            tr("There is not enough free space to stage this device media copy"));
        return;
    }

    const QString partialPath =
        outputPath + QStringLiteral(".part");
    QFile partial(partialPath);
    if (!partial.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        ::fchmod(partial.handle(), S_IRUSR | S_IWUSR) != 0) {
        errorMessage = tr("Cannot create the private recovered media artifact: %1")
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
            emit printerForegroundProgress(
                operationId, QStringLiteral("PullingDeviceMedia"),
                bytesDecoded, totalBytes,
                tr("Reading and decoding the device media copy..."),
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
        errorMessage = tr(
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
        errorMessage = tr(
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
            tr("Published recovered media artifact failed its final filesystem validation"));
        return;
    }

    restartPrinterKeepaliveAfterActivity();
    finish(true, false, result.fileSize, result.chunkCount,
           result.rawSha256, result.decodedSha256, {});
}

void DeviceWorker::preflightReplacePrinterMedia(
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
        emit printerReplacePreflightFinished(
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
        emit printerReplacePreflightFinished(
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
    emit printerReplacePreflightFinished(
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

void DeviceWorker::deletePrinterMedia(
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
        emit printerDeleteFinished(
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
        emit printerDeleteFinished(
            operationId, fileNames, {}, {}, false,
            reconcileOnly
                ? PrinterProtocol::MutationOutcome::PartialOrUnknown
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }

    PrinterProtocol::OperationContext stableContext;
    stableContext.cancellationFd = printerCancellationFd_;
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
                    *persistenceError = tr(
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
                        ? tr("Delete intent no longer matches this operation")
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
            emit printerForegroundProgress(
                operationId, stage, completedFiles, totalFiles,
                stage == QStringLiteral("DeletePreflight")
                    ? tr("Checking whether %1 can be deleted...")
                          .arg(fileName)
                    : stage == QStringLiteral("Deleting")
                        ? tr("Sending one delete request for %1...")
                              .arg(fileName)
                        : tr("Verifying deletion of %1 through FileList...")
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
    emit printerDeleteFinished(
        operationId, fileNames, result.deletedNames, result.files,
        result.success, result.outcome, result.error, generation);
}

void DeviceWorker::applyPrinterMedia(const QString &devicePath,
                                     const QString &mediaFile,
                                     const TryxRuntimeApplyRequest &request,
                                     bool updateMetrics,
                                     const QString &proofDeviceIdentity,
                                     const QList<TryxRuntimeSavedMediaRefV1> &proof,
                                     const QString &operationId,
                                     quint64 generation) {
    synchronizePublishedPresentationPreferences();
    const bool savedLayoutApply = !proof.isEmpty();
    if (savedLayoutApply && !printerGenerationIsCurrent(generation)) {
        emit printerSavedLayoutProofFailed(
            operationId, QStringLiteral("DeviceGenerationChanged"),
            tr("The USB device changed before the saved layout could be verified"),
            generation);
        return;
    }
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        if (savedLayoutApply &&
            !printerOperationIsCancelled(operationId)) {
            emit printerSavedLayoutProofFailed(
                operationId,
                printerGenerationIsCurrent(generation)
                    ? QStringLiteral("FileListUnavailable")
                    : QStringLiteral("DeviceGenerationChanged"),
                errorMessage, generation);
            return;
        }
        emit printerApplyFinished(
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
                emit printerApplyFinished(
                    operationId, mediaFile, false, updateMetrics,
                    PrinterProtocol::MutationOutcome::Cancelled,
                    tr("The saved layout Apply was cancelled by the user"),
                    generation);
                return;
            }
            if (!printerGenerationIsCurrent(generation)) {
                emit printerSavedLayoutProofFailed(
                    operationId, QStringLiteral("DeviceGenerationChanged"),
                    tr("The USB device changed before the saved layout could be verified"),
                    generation);
                return;
            }
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("FileListUnavailable"),
                errorMessage, generation);
            schedulePrinterSessionRecovery(errorMessage, generation);
            return;
        }
        emit printerApplyFinished(
            operationId, mediaFile, false, updateMetrics,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        schedulePrinterSessionRecovery(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    if (savedLayoutApply) {
        emit printerForegroundProgress(
            operationId, QStringLiteral("VerifyingSavedLayout"), 0, 0,
            tr("Verifying saved layout media through FileList..."),
            generation);
        const PrinterProtocol::MediaListResult current =
            printerProtocol_->readMediaList(devicePath, context);
        if (!current.success) {
            if (printerOperationIsCancelled(operationId)) {
                emit printerApplyFinished(
                    operationId, mediaFile, false, updateMetrics,
                    PrinterProtocol::MutationOutcome::Cancelled,
                    tr("The saved layout Apply was cancelled by the user"),
                    generation);
                return;
            }
            if (!printerGenerationIsCurrent(generation)) {
                emit printerSavedLayoutProofFailed(
                    operationId, QStringLiteral("DeviceGenerationChanged"),
                    tr("The USB device changed before the saved layout could be verified"),
                    generation);
                return;
            }
            schedulePrinterSessionRecovery(current.error, generation);
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("FileListUnavailable"),
                tr("The saved layout media could not be verified: %1")
                    .arg(current.error),
                generation);
            return;
        }
        if (printerOperationIsCancelled(operationId)) {
            emit printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                tr("The USB device changed after the saved layout FileList proof"),
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
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("SavedLayoutMediaChanged"),
                tr("The saved layout media changed in the fresh FileList; no display mutation was sent"),
                generation);
            return;
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (savedLayoutProofConfirmedHookForTesting_) {
            savedLayoutProofConfirmedHookForTesting_();
        }
#endif
        if (printerOperationIsCancelled(operationId)) {
            emit printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                tr("The USB device changed after the saved layout FileList proof"),
                generation);
            return;
        }
    }

    const QString operationSubject = mediaFile.isEmpty()
        ? tr("display settings")
        : mediaFile;
    emit printerUploadProgress(
        tr("Applying printer-class configuration: %1")
            .arg(operationSubject),
        generation);
    emit printerForegroundProgress(
        operationId, QStringLiteral("Applying"), 0, 0,
        tr("Applying printer-class configuration: %1")
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
            emit printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                tr("The USB device changed after the saved layout FileList proof"),
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
            emit printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                tr("The USB device changed after the saved layout FileList proof"),
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
            emit printerDisplayStateReady(
                appliedState, generation);
        }
        const bool requiresSessionRecovery =
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::VerificationFailed &&
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::Rejected;
        emit printerApplyFinished(
            operationId, mediaFile, false, rebuildOverlay,
            mutationDetails.outcome,
            tr("Failed to apply printer-class configuration: %1")
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
    emit printerDisplayStateReady(appliedState, generation);
    emit printerUploadProgress(
        tr("Printer-class configuration applied"), generation);
    emit printerApplyFinished(
        operationId, mediaFile, true, rebuildOverlay,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation);
    restartPrinterKeepaliveAfterActivity();
}

void DeviceWorker::configurePrinterMetrics(
    const QString &devicePath,
    const TryxRuntimeMetricsConfigRequest &request,
    const QString &operationId, quint64 generation) {
    synchronizePublishedPresentationPreferences();
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerMetricsConfigured(
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
        emit printerMetricsConfigured(
            operationId, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    emit printerForegroundProgress(
        operationId, QStringLiteral("ConfiguringMetrics"), 0, 0,
        tr("Configuring PASE metrics layout..."), generation);
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
        emit printerMetricsConfigured(
            operationId, false, mutationDetails.outcome,
            tr("Failed to configure PASE metrics: %1").arg(errorMessage),
            generation);
        return;
    }
    printerOverlayConfig_ = overlay;
    printerGpuPin_ = candidateGpuPin;
    printerOverlayLeaseRefreshNext_ = false;
    startPrinterMetrics();
    emit printerMetricsConfigured(
        operationId, true, PrinterProtocol::MutationOutcome::Succeeded,
        QString(), generation);
    restartPrinterKeepaliveAfterActivity();
}

void DeviceWorker::sendPrinterSysinfo(
    const QString &devicePath, const QStringList &labels,
    const QStringList &values, const QStringList &units,
    quint64 generation) {
    synchronizePublishedPresentationPreferences();
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        emit printerSysinfoSent(generation);
        return;
    }
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage) ||
        !ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    if (!printerProtocol_->sendPaseMetricBatch(
            devicePath, printerOverlayConfig_, labels, values, units,
            &errorMessage, context)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }
    updatePrinterOverlayInitialMetrics(labels, values, units);
    restartPrinterKeepaliveAfterActivity();
    emit printerSysinfoSent(generation);
}

void DeviceWorker::startPrinterMetrics() {
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

void DeviceWorker::sendPrinterMetrics() {
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
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }

    updatePrinterOverlayInitialMetrics(labels, values, units);
    // Metric updates are headerless KANALI UI commands, not the UDB watchdog
    // Ping. Keep the independent two-second Ping schedule even while one-second
    // metric samples are active.
    emit printerSysinfoSent(generation);
}

void DeviceWorker::collectCurrentPrinterMetrics(QStringList *labels,
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

void DeviceWorker::publishPrinterMetricsAvailability(
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
    emit printerMetricsAvailabilityChanged(
        labels, configuredPrinterGeneration_);
}

void DeviceWorker::updatePrinterOverlayInitialMetrics(
    const QStringList &labels, const QStringList &values,
    const QStringList &units) {
    setPaseOverlayInitialMetrics(
        &printerOverlayConfig_, labels, values, units);
}

void DeviceWorker::startPrinterDisplaySession(const QString &devicePath,
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

void DeviceWorker::retryPrinterSessionStart() {
    if (printerSessionState_ != PrinterSessionState::Recovering) {
        return;
    }
    markPrinterSessionLost(
        tr("Automatic same-generation PASE bootstrap retry is disabled; physically reconnect the device before continuing"),
        configuredPrinterGeneration_);
}

void DeviceWorker::attemptPrinterSessionStart(const QString &devicePath,
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
                ? tr("PASE display session bootstrap failed; physically reconnect the device before continuing")
                : tr("PASE display session bootstrap failed: %1. Physically reconnect the device before continuing")
                      .arg(errorMessage),
            generation);
        return;
    }
    printerSessionRecoveryAttempt_ = 0;
}

void DeviceWorker::sendPrinterKeepalive() {
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
                tr("PASE keepalive was cancelled before a safe write could start"),
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
        emit printerUploadProgress(
            tr("Printer-class keepalive write will be retried (%1/%2): %3")
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
                    ? tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed")
                    : tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed: %1")
                          .arg(errorMessage),
                generation);
            return;
        }
        const QString stoppedMessage =
            outcome == PrinterProtocol::KeepaliveOutcome::RetryableFailure
            ? (refreshOverlayLease
                   ? tr("PASE overlay lease refresh stopped after %1 retries: %2")
                   : tr("Printer-class keepalive stopped after %1 retries: %2"))
                  .arg(kMaxPrinterKeepaliveWriteRetries)
                  .arg(errorMessage)
            : (refreshOverlayLease
                   ? tr("PASE overlay lease refresh stopped: %1")
                   : tr("Printer-class keepalive stopped: %1"))
                  .arg(errorMessage);
        if (refreshOverlayLease) {
            markPrinterSessionLost(
                tr("PASE overlay lease could not be refreshed safely: %1")
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
    emit printerTransportReady(generation);
    if (recovered) {
        emit printerUploadProgress(
            tr("PASE display keepalive recovered"), generation);
    }
}

bool DeviceWorker::preparePrinterOperation(
    const QString &devicePath, quint64 generation,
    const QString &operationId,
    PrinterProtocol::OperationContext *context, QString *errorMessage) {
    if (devicePath.isEmpty() || devicePath != printerDevicePath_ ||
        generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        if (errorMessage) {
            *errorMessage =
                tr("Printer-class operation was cancelled because the USB device changed");
        }
        return false;
    }

    if (printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = tr("Printer-class operation was cancelled by the user");
        }
        return false;
    }
    const int cancellationFd = operationId.isEmpty()
        ? printerCancellationFd_
        : printerOperationCancellationFd_;
    drainPrinterCancellation(cancellationFd);
    if (!printerGenerationIsCurrent(generation) ||
        printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = printerOperationIsCancelled(operationId)
                ? tr("Printer-class operation was cancelled by the user")
                : tr("Printer-class operation was cancelled because the USB device changed");
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

bool DeviceWorker::ensurePrinterSession(
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
            *errorMessage = tr(
                "The PASE protocol session is waiting for confirmed overlay restoration");
        }
        return false;
    }
    if (printerSessionState_ ==
            PrinterSessionState::AwaitingProtocolReadiness ||
        printerSessionState_ == PrinterSessionState::Starting) {
        if (errorMessage) {
            *errorMessage = tr("Printer-class session is already starting");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Lost) {
        if (errorMessage) {
            *errorMessage = tr(
                "Printer-class session is lost until a new USB endpoint generation appears");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Recovering) {
        if (errorMessage) {
            *errorMessage = tr(
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
    emit printerUploadProgress(
        transferOnly
            ? tr("Opening the TRYX transfer session...")
            : tr("Starting PASE display session..."),
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
            const QString persistentMessage = tr(
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
                ? tr("Failed to start the printer-class session")
                : tr("Failed to start the printer-class session: %1")
                      .arg(sessionResult.error);
        }
        return false;
    }

    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage =
                tr("Printer-class session was cancelled because the USB device changed");
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
            *errorMessage = tr(
                "PASE bootstrap completed without an exact DeviceInfo readiness confirmation");
        }
        return false;
    }
    if (!transferOnly) {
        pendingPrinterDeviceSpecifications_ =
            sessionResult.deviceSpecifications;
        printerDeviceSpecificationsPending_ = true;
    }
    emit printerDeviceVersionsReady(
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
        emit printerSessionStarted(generation);
        emit printerUploadProgress(
            tr("TRYX transfer session is ready"), generation);
        emit printerTransportReady(generation);
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
    emit printerUploadProgress(
        tr("PASE protocol session is ready; waiting for a confirmed keepalive before restoring the overlay"),
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
            ? tr("The mandatory post-bootstrap PASE keepalive failed")
            : tr("The mandatory post-bootstrap PASE keepalive did not activate the display session");
    }
    return false;
}

bool DeviceWorker::printerGenerationIsCurrent(quint64 generation) const {
    return printerEndpointReady_.load(std::memory_order_acquire) &&
           printerGenerationGate_.load(std::memory_order_acquire) == generation;
}

bool DeviceWorker::printerOperationIsCancelled(
    const QString &operationId) const {
    if (operationId.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&printerOperationCancellationMutex_);
    return cancelledPrinterOperationIds_.contains(operationId);
}

void DeviceWorker::drainPrinterCancellation(int cancellationFd) {
    if (cancellationFd < 0) {
        return;
    }
    uint64_t value = 0;
    while (::read(cancellationFd, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
}

void DeviceWorker::drainAllPrinterCancellations() {
    drainPrinterCancellation(printerCancellationFd_);
    drainPrinterCancellation(printerOperationCancellationFd_);
}

QString DeviceWorker::printerSessionStateName(
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

void DeviceWorker::transitionPrinterSessionState(
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

void DeviceWorker::stopPrinterSession() {
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
        emit printerSessionStopped(stoppedGeneration);
    }
}

void DeviceWorker::schedulePrinterSessionRecovery(
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
            ? tr("The PASE USB interface stopped responding after persistent input transport errors. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
            : tr("The PASE USB interface stopped responding after persistent input transport errors: %1. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
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
            ? tr("The PASE display session stopped after an operation failure; physically reconnect the device before continuing")
            : tr("The PASE display session stopped after an operation failure: %1. Physically reconnect the device before continuing")
                  .arg(reason),
        generation);
}

void DeviceWorker::restartPrinterKeepaliveAfterActivity() {
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

void DeviceWorker::activateRestoredPrinterOverlay(
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
                tr("PASE overlay restoration was cancelled because the USB generation changed"),
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
                ? tr("PASE overlay restoration failed")
                : tr("PASE overlay restoration failed: %1")
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
    emit printerSessionStarted(generation);
    emit printerUploadProgress(
        restoreOverlay
            ? tr("PASE display session and overlay are active")
            : tr("PASE display session is active"),
        generation);
    restartPrinterKeepaliveAfterActivity();
    emit printerTransportReady(generation);
}

void DeviceWorker::publishPendingPrinterDeviceSpecifications(
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

    emit printerDeviceSpecificationsReady(
        specifications, printerDevicePath_, printerDeviceSerial_,
        printerProductId_, generation);
}

void DeviceWorker::markPrinterSessionLost(
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
        ? tr("The PASE display session is lost until a new USB endpoint generation appears")
        : reason;
    emit printerOperationError(message, generation);
    emit printerSessionLost(generation);
}

void DeviceWorker::sendKeepalive() {
    if (!device_ || !device_->is_connected()) {
        return;
    }
    device_->handshake();
}

void DeviceWorker::setRotation(int degrees) {
    if (!device_ || !device_->is_connected()) {
        return;
    }
    device_->set_rotation(degrees);
}

void DeviceWorker::rebootDevice() {
    // ADB reboot works, POST reboot doesn't
    std::system("adb -s $(adb devices 2>/dev/null | grep TRYX | cut -f1) reboot 2>/dev/null");
}

// --- DeviceManager ---

DeviceManager::DeviceManager(QObject *parent)
    : DeviceManager(new PrinterDeviceMonitor, true, parent) {}

void DeviceManager::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerOverlayLeaseMode_ = mode;
    if (!worker_) {
        return;
    }
    if (worker_->thread() == QThread::currentThread()) {
        worker_->setPrinterOverlayLeaseMode(mode);
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, mode]() {
            worker->setPrinterOverlayLeaseMode(mode);
        },
        Qt::QueuedConnection);
}

bool DeviceManager::setPresentationPreferences(
    quint64 expectedRevision, const QString &temperatureUnit,
    const QString &timeFormat,
    TryxRuntimePresentationPreferencesV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    if (confirmed) {
        *confirmed = presentationPreferences_;
    }
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = presentationPreferences_;
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };

    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradeV10Prepared"),
            tr("Device mutations are blocked because runtime downgrade preparation is committed"));
    }

    if (!tryxTemperatureUnitIsValid(temperatureUnit) ||
        !tryxTimeFormatIsValid(timeFormat)) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidPresentationPreferences"),
            tr("The presentation preferences are invalid"));
    }
    if (temperatureUnit == presentationPreferences_.temperatureUnit &&
        timeFormat == presentationPreferences_.timeFormat) {
        return true;
    }
    if (expectedRevision != presentationPreferences_.revision) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesConflict"),
            tr("The presentation preferences changed; refresh them before saving again"));
    }
    if (presentationPreferences_.revision ==
        std::numeric_limits<quint64>::max()) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesConflict"),
            tr("The presentation preference revision is exhausted; restart the runtime before saving again"));
    }
    if (!runtimePresentationPreferencesStore_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesPersistenceFailed"),
            tr("The presentation preferences store is unavailable"));
    }
    const auto persisted = runtimePresentationPreferencesStore_->persist(
        temperatureUnit, timeFormat);
    if (!persisted.ok()) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesPersistenceFailed"),
            persisted.detail.isEmpty()
                ? tr("The presentation preferences could not be saved")
                : persisted.detail);
    }

    presentationPreferences_.temperatureUnit = temperatureUnit;
    presentationPreferences_.timeFormat = timeFormat;
    ++presentationPreferences_.revision;
    const TryxRuntimePresentationPreferencesV1 published =
        presentationPreferences_;
    worker_->publishPresentationPreferences(published);
    if (worker_->thread() == QThread::currentThread()) {
        worker_->applyPublishedPresentationPreferences();
    } else {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() {
                worker->applyPublishedPresentationPreferences();
            },
            Qt::QueuedConnection);
    }
    if (confirmed) {
        *confirmed = published;
    }
    emit presentationPreferencesChanged(published);
    return true;
}

TryxRuntimeSavedLayoutsSnapshotV1
DeviceManager::savedLayoutsSnapshot() const {
    TryxRuntimeSavedLayoutsSnapshotV1 snapshot;
    snapshot.revision = savedLayoutStore_
        ? savedLayoutStore_->revision()
        : 0;
    if (!connected_ || !printerClassConnected_) {
        snapshot.status = QStringLiteral("Disconnected");
        return snapshot;
    }

    snapshot.deviceIdentity = printerDeviceSerial_;
    snapshot.productId = printerProductIdString(printerProductId_);
    const auto profile = currentPrinterProductProfile();
    if (snapshot.deviceIdentity.isEmpty()) {
        snapshot.status = QStringLiteral("Disconnected");
        snapshot.productId.clear();
        return snapshot;
    }
    if (!tryxSavedLayoutDeviceIdentityIsCanonical(
            snapshot.deviceIdentity)) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = tr(
            "The saved layout device identity is invalid");
        return snapshot;
    }
    if (!profile ||
        (profile->productId != 0x1011 && profile->productId != 0x1021) ||
        !profile->mediaCatalogSupported ||
        !profile->displayConfigurationSupported ||
        !profile->overlayMetricsSupported) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }
    if (!savedLayoutStore_ || !savedLayoutsStoreLoaded_ ||
        !savedLayoutStore_->writesEnabled()) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = savedLayoutsFailureDetail_.isEmpty()
            ? tr("Saved layouts are unavailable")
            : savedLayoutsFailureDetail_.left(512);
        return snapshot;
    }

    QString exactIdentity;
    QString exactProduct;
    if (!currentSavedLayoutsContext(
            &exactIdentity, &exactProduct)) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = tr(
            "Saved layouts are waiting for the current device handshake");
        return snapshot;
    }
    return savedLayoutStore_->snapshot(exactIdentity, exactProduct);
}

bool DeviceManager::putSavedLayout(
    quint64 expectedSnapshotRevision,
    const TryxRuntimeSavedLayoutV1 &layout,
    TryxRuntimeSavedLayoutsSnapshotV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = savedLayoutsSnapshot();
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };

    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradePrepared"),
            tr("Saved layouts are locked after runtime downgrade preparation"));
    }
    const TryxRuntimeSavedLayoutsSnapshotV1 current =
        savedLayoutsSnapshot();
    if (current.status != QStringLiteral("Ready")) {
        return fail(
            current.status == QStringLiteral("Unsupported")
                ? QStringLiteral(
                      "org.tryx.Panorama.Error.UnsupportedProduct")
                : QStringLiteral(
                      "org.tryx.Panorama.Error.SavedLayoutsUnavailable"),
            current.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : current.diagnostic);
    }
    if (layout.deviceIdentity != current.deviceIdentity ||
        layout.productId != current.productId) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidSavedLayout"),
            tr("The saved layout belongs to another device"));
    }
    QList<TryxRuntimeSavedMediaRefV1> proof;
    QString validationError;
    if (!buildSavedLayoutMediaProof(
            layout.request, &proof, &validationError) ||
        proof != layout.media) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidSavedLayout"),
            validationError.isEmpty()
                ? tr("The saved layout media does not match the current catalog")
                : validationError);
    }

    const tryx::SavedLayoutStore::MutationResult result =
        savedLayoutStore_->put(expectedSnapshotRevision, layout);
    if (!result.ok()) {
        if (result.commitMayExist ||
            result.code ==
                tryx::SavedLayoutStore::ErrorCode::CommitUnknown) {
            savedLayoutsFailureDetail_ = result.detail.left(512);
        }
        return fail(
            savedLayoutStoreErrorName(result.code),
            result.detail.isEmpty()
                ? tr("The saved layout could not be stored")
                : result.detail);
    }
    if (confirmed) {
        *confirmed = savedLayoutsSnapshot();
    }
    return true;
}

bool DeviceManager::deleteSavedLayout(
    quint64 expectedSnapshotRevision, const QString &layoutId,
    TryxRuntimeSavedLayoutsSnapshotV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = savedLayoutsSnapshot();
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };
    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradePrepared"),
            tr("Saved layouts are locked after runtime downgrade preparation"));
    }
    const TryxRuntimeSavedLayoutsSnapshotV1 current =
        savedLayoutsSnapshot();
    if (current.status != QStringLiteral("Ready")) {
        return fail(
            current.status == QStringLiteral("Unsupported")
                ? QStringLiteral(
                      "org.tryx.Panorama.Error.UnsupportedProduct")
                : QStringLiteral(
                      "org.tryx.Panorama.Error.SavedLayoutsUnavailable"),
            current.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : current.diagnostic);
    }
    const tryx::SavedLayoutStore::MutationResult result =
        savedLayoutStore_->remove(
            expectedSnapshotRevision, current.deviceIdentity,
            current.productId, layoutId);
    if (!result.ok()) {
        if (result.commitMayExist ||
            result.code ==
                tryx::SavedLayoutStore::ErrorCode::CommitUnknown) {
            savedLayoutsFailureDetail_ = result.detail.left(512);
        }
        return fail(
            savedLayoutStoreErrorName(result.code),
            result.detail.isEmpty()
                ? tr("The saved layout could not be deleted")
                : result.detail);
    }
    if (confirmed) {
        *confirmed = savedLayoutsSnapshot();
    }
    return true;
}

bool DeviceManager::acquireFirmwareExclusive(
    const QString &leaseId, QString *errorMessage) {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    if (QThread::currentThread() != thread()) {
        return fail(tr(
            "The firmware transport gate must be acquired on the runtime thread"));
    }
    if (!worker_ || !workerThread_.isRunning()) {
        return fail(tr(
            "The local device transport is unavailable for firmware flashing"));
    }
    QString productError;
    if (!firmwareFlashAllowedForCurrentDevice(&productError)) {
        return fail(productError);
    }
    const QString normalizedLease = leaseId.trimmed();
    if (normalizedLease.isEmpty()) {
        return fail(tr("The firmware transport lease is invalid"));
    }
    if (firmwareExclusiveActive()) {
        return fail(tr(
            "Another firmware operation already owns the device transport"));
    }
    if (!activeOperationId_.isEmpty()) {
        return fail(
            tr("Device operation %1 is still active")
                .arg(activeOperationId_));
    }
    if (retryCacheMutationGateActive()) {
        return fail(tr(
            "Stored retry media is still being validated or requires recovery"));
    }
    const QString retryOperationId = retryCacheVisibleOperationId();
    if (!retryOperationId.isEmpty()) {
        const auto retry = operations_.constFind(retryOperationId);
        if (retry == operations_.constEnd() ||
            retry->requiresDeviceRecovery ||
            retry->uploadFinalizationReconciliationPending ||
            retry->info.terminalOutcome ==
                QStringLiteral("PartialOrUnknown") ||
            retry->info.terminalOutcome ==
                QStringLiteral("FinalizationUnknown")) {
            return fail(tr(
                "A previous media transfer has an unresolved device outcome; cancel or reconcile it before firmware flashing"));
        }
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        QFileInfo::exists(deleteIntentPath())) {
        return fail(tr(
            "A previous delete command still requires read-only reconciliation"));
    }
    if (!pendingReplaceJournalOperationId_.isEmpty() ||
        QFileInfo::exists(replaceIntentPath())) {
        return fail(tr(
            "A previous replacement still requires read-only reconciliation"));
    }
    if (printerRecoveryRequired_) {
        return fail(tr(
            "The PASE requires physical reconnect recovery before firmware flashing"));
    }
    if (printerDisplaySessionLost_) {
        return fail(tr(
            "The PASE display session is lost; physically reconnect the device before firmware flashing"));
    }

    // All public device entry points run on this thread. Publishing the lease
    // before closing the worker generation gate makes the active-operation
    // check and mutation exclusion one indivisible event-loop transition.
    firmwareExclusiveLeaseId_ = normalizedLease;
    firmwareRecoveryReconnectRequested_ = false;
    firmwareResumeAutoConnect_ = autoConnectMode_;
    clearDeviceSpecificationsCache();
    firmwareQuiesceGeneration_ = ++printerGeneration_;
    setPrinterDisplaySessionActive(false);
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    printerSessionResumeProductId_ = 0;
    stopKeepalive();
    emit requestCancelPrinterPreparation(printerGeneration_);
    worker_->updatePrinterGenerationGate(printerGeneration_, false);
    emit requestFirmwareTransportQuiesce(
        normalizedLease, firmwareQuiesceGeneration_);
    emit uploadStatus(tr(
        "Device transport is reserved for firmware flashing"));
    return true;
}

void DeviceManager::releaseFirmwareExclusive(
    const QString &leaseId, bool resumeTransport) {
    if (QThread::currentThread() != thread() ||
        leaseId.trimmed().isEmpty() ||
        leaseId.trimmed() != firmwareExclusiveLeaseId_) {
        return;
    }
    if (!firmwareReleasePendingLeaseId_.isEmpty()) {
        if (firmwareReleasePendingLeaseId_ ==
            leaseId.trimmed()) {
            // A later shutdown request may downgrade an already queued resume.
            firmwareReleaseResumeTransport_ =
                firmwareReleaseResumeTransport_ &&
                resumeTransport;
        }
        return;
    }
    firmwareReleasePendingLeaseId_ =
        leaseId.trimmed();
    firmwareReleaseResumeTransport_ =
        resumeTransport && firmwareResumeAutoConnect_;
    emit requestFirmwareQuiesceReleaseFence(
        firmwareReleasePendingLeaseId_,
        firmwareQuiesceGeneration_);
}

void DeviceManager::
    setFirmwareRecoveryInterlockActive(
        bool active) {
    firmwareRecoveryInterlockActive_ = active;
    if (active) {
        autoConnectMode_ = false;
        stopKeepalive();
    }
}

void DeviceManager::
    resumeConnectionAfterFirmwareRecoveryAcknowledgement() {
    if (firmwareRecoveryInterlockActive_) {
        emit deviceError(tr(
            "Device connection remains blocked by firmware recovery"));
        return;
    }
    if (firmwareExclusiveActive()) {
        // A firmware completion publishes its recovery state before the
        // worker-thread release fence necessarily returns. Preserve this
        // explicit user action and reconnect only after the old transport
        // queue is proven empty.
        firmwareRecoveryReconnectRequested_ = true;
        return;
    }
    connectDevice();
}

DeviceManager::DeviceManager(PrinterDeviceMonitor *printerMonitor,
                             bool startPrinterMonitor, QObject *parent)
    : QObject(parent),
      worker_(new DeviceWorker),
      printerMediaPreparer_(new PrinterMediaPreparer),
      keepaliveTimer_(new QTimer(this)),
      printerMonitor_(printerMonitor),
      mediaCatalogStore_(
          std::make_unique<tryx::MediaCatalogStore>()),
      paseMetricsConfigStore_(
          std::make_unique<tryx::PaseMetricsConfigStore>()),
      runtimePresentationPreferencesStore_(
          std::make_unique<tryx::RuntimePresentationPreferencesStore>()),
      savedLayoutStore_(
          std::make_unique<tryx::SavedLayoutStore>()),
      runtimeDowngradeStore_(
          std::make_unique<tryx::RuntimeDowngradeStore>()),
      deviceMediaArtifactStore_(
          std::make_unique<tryx::DeviceMediaArtifactStore>()) {
    automaticPrinterSessionStart_ = startPrinterMonitor;
    printerMonitor_->setParent(this);
    qRegisterMetaType<PrinterProtocol::UsbPrinterDevice>();
    qRegisterMetaType<PrinterProtocol::DiscoverySnapshot>();
    qRegisterMetaType<PrinterProtocol::DeviceInfo>();
    qRegisterMetaType<PrinterProtocol::DeviceSpecifications>();
    qRegisterMetaType<PrinterProtocol::MediaFile>();
    qRegisterMetaType<QList<PrinterProtocol::MediaFile>>();
    qRegisterMetaType<PrinterProtocol::MutationOutcome>();
    qRegisterMetaType<PrinterProtocol::PaseOverlayConfig>();
    qRegisterMetaType<PrinterProtocol::PaseDisplayState>();
    qRegisterMetaType<TryxRuntimeOperationInfo>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayState>();
    qRegisterMetaType<TryxRuntimeDeviceMediaArtifact>();
    qRegisterMetaType<TryxRuntimePresentationPreferencesV1>();
    qRegisterMetaType<QList<TryxRuntimeSavedMediaRefV1>>();
    qRegisterMetaType<RecoveredH264ProbeMetadata>();

    artifactOwnerWatcher_ = new QDBusServiceWatcher(this);
    artifactOwnerWatcher_->setConnection(
        QDBusConnection::sessionBus());
    artifactOwnerWatcher_->setWatchMode(
        QDBusServiceWatcher::WatchForUnregistration);
    connect(
        artifactOwnerWatcher_,
        &QDBusServiceWatcher::serviceUnregistered,
        this, &DeviceManager::handleArtifactOwnerUnregistered);
    artifactSweepTimer_ = new QTimer(this);
    artifactSweepTimer_->setInterval(kDeviceMediaSweepIntervalMs);
    connect(
        artifactSweepTimer_, &QTimer::timeout,
        this, &DeviceManager::sweepDeviceMediaArtifacts);

    QString overlayLeaseConfigStatus =
        QStringLiteral("test-default");
    if (startPrinterMonitor) {
        std::error_code configPathError;
        const bool configFileExists = std::filesystem::exists(
            panorama::ConfigManager::get_config_path(),
            configPathError);
        const auto config = panorama::ConfigManager::load_config();
        if (!config) {
            overlayLeaseConfigStatus =
                QStringLiteral("unreadable-or-invalid");
            qWarning().noquote()
                << "Invalid TRYX config.json; using"
                << printerOverlayLeaseModeName(
                       PrinterOverlayLeaseMode::
                           PingAndOverlayLease)
                << "for the PASE overlay lease mode";
        } else if (config->pase_overlay_lease_mode ==
                   "ping-only") {
            printerOverlayLeaseMode_ =
                PrinterOverlayLeaseMode::PingOnly;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        } else {
            printerOverlayLeaseMode_ =
                PrinterOverlayLeaseMode::PingAndOverlayLease;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        }
    }
    setPrinterOverlayLeaseMode(printerOverlayLeaseMode_);
    if (startPrinterMonitor) {
        loadRuntimePresentationPreferences();
    }
    if (startPrinterMonitor) {
        logPrinterLifecycleEvent(
            QStringLiteral("overlay_lease_mode_selected"),
            printerGeneration_,
            {
                {QStringLiteral("lease_mode"),
                 printerOverlayLeaseModeName(
                     printerOverlayLeaseMode_)},
                {QStringLiteral("config_status"),
                 overlayLeaseConfigStatus}
            });
    }

    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    printerMediaPreparer_->moveToThread(&printerPreparationThread_);
    connect(&printerPreparationThread_, &QThread::finished,
            printerMediaPreparer_, &QObject::deleteLater);

    connect(worker_, &DeviceWorker::connected, this,
            [this](const QString &pid, const QString &serial,
                   const QString &fw, const QString &app) {
                if (runtimeDowngradeV10Prepared_ ||
                    firmwareExclusiveActive() ||
                    firmwareRecoveryInterlockActive_) {
                    return;
                }
                if (printerSnapshot_.blocksLegacyTransport()) {
                    emit requestDisconnect();
                    return;
                }
                legacyProductId_ = pid.trimmed();
                connected_ = true;
                printerClassConnected_ = false;
                emit deviceConnected(pid, serial, fw, app);
            });
    connect(worker_, &DeviceWorker::disconnected, this, [this]() {
        if (runtimeDowngradeV10Prepared_ ||
            printerClassConnected_ || firmwareExclusiveActive()) {
            return;
        }
        legacyProductId_.clear();
        connected_ = false;
        emit deviceDisconnected();
    });
    const auto legacyResultIsCurrent = [this]() {
        return !runtimeDowngradeV10Prepared_ &&
               !firmwareExclusiveActive() &&
               !firmwareRecoveryInterlockActive_ &&
               connected_ && !printerClassConnected_ &&
               !printerSnapshot_.blocksLegacyTransport();
    };
    connect(worker_, &DeviceWorker::error, this, [this](const QString &message) {
        if (!runtimeDowngradeV10Prepared_ &&
            !firmwareExclusiveActive() &&
            !firmwareRecoveryInterlockActive_ &&
            !printerSnapshot_.blocksLegacyTransport()) {
            emit deviceError(message);
        }
    });
    connect(worker_, &DeviceWorker::brightnessSet, this,
            [this, legacyResultIsCurrent](int value) {
                if (legacyResultIsCurrent()) {
                    emit brightnessChanged(value);
                }
            });
    connect(worker_, &DeviceWorker::screenConfigSet, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit screenConfigChanged();
                }
            });
    connect(worker_, &DeviceWorker::mediaUploaded, this,
            [this, legacyResultIsCurrent](const QString &fileName) {
                if (legacyResultIsCurrent()) {
                    emit mediaUploaded(fileName);
                }
            });
    connect(worker_, &DeviceWorker::mediaDeleted, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit mediaDeleted();
                }
            });
    connect(worker_, &DeviceWorker::mediaListReady, this,
            [this, legacyResultIsCurrent](const QStringList &files) {
                if (legacyResultIsCurrent()) {
                    emit mediaListUpdated(files);
                }
            });
    connect(worker_, &DeviceWorker::uploadProgress, this,
            [this, legacyResultIsCurrent](const QString &status) {
                if (legacyResultIsCurrent()) {
                    emit uploadStatus(status);
                }
            });

    const auto printerResultIsCurrent = [this](quint64 generation) {
        return !runtimeDowngradeV10Prepared_ &&
               !firmwareExclusiveActive() &&
               !firmwareRecoveryInterlockActive_ &&
               generation == printerGeneration_ && printerClassConnected_ &&
               printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready;
    };
    const auto printerOperationResultIsExpected =
        [this, printerResultIsCurrent](const QString &operationId,
                                       quint64 generation) {
            const auto found = operations_.constFind(operationId);
            if (operationId.isEmpty() ||
                activeOperationId_ != operationId ||
                found == operations_.constEnd() ||
                found->info.deviceGeneration != generation) {
                return false;
            }
            return (printerResultIsCurrent(generation) &&
                    operationMatchesCurrentPrinterProduct(*found)) ||
                   found->deviceChangePending;
        };
    connect(worker_, &DeviceWorker::printerOperationError, this,
            [this, printerResultIsCurrent](const QString &message, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit deviceError(message);
                }
            });
    connect(worker_, &DeviceWorker::printerUploadProgress, this,
            [this, printerResultIsCurrent](const QString &status, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit uploadStatus(status);
                }
            });
    connect(worker_, &DeviceWorker::printerSessionStarted, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                if (retryCacheRestrictedRecoveryActive()) {
                    setPrinterDisplaySessionActive(false);
                    startRetryCacheReadOnlyReconciliationIfReady();
                    emit uploadStatus(printerMutationUnavailableStatusText());
                    return;
                }
                if (printerRecoveryRequired_ ||
                    retryCacheStartupSessionGateActive()) {
                    setPrinterDisplaySessionActive(false);
                    emit uploadStatus(printerMutationUnavailableStatusText());
                    return;
                }
                const QString sysfsPath =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().sysfsPath
                        : QString();
                logPrinterLifecycleEvent(
                    QStringLiteral("recovery_completed"), generation,
                    {
                        {QStringLiteral("sysfs_path"), sysfsPath},
                        {QStringLiteral("serial"),
                         printerDeviceSerial_.trimmed()},
                        {QStringLiteral("disconnect_count"),
                         QString::number(printerDisconnectCount_)},
                        {QStringLiteral("elapsed_ms"),
                         printerGenerationElapsedTimer_.isValid()
                             ? QString::number(
                                   printerGenerationElapsedTimer_
                                       .elapsed())
                             : QStringLiteral("-1")},
                        {QStringLiteral("lease_mode"),
                         printerOverlayLeaseModeName(
                             printerOverlayLeaseMode_)}
                    });
                printerDisplaySessionLost_ = false;
                printerSessionLossRemovalObserved_ = false;
                setPrinterDisplaySessionActive(true);
                printerSessionResumePending_ = false;
                printerSessionResumeSerial_.clear();
                printerSessionResumeProductId_ = 0;
                if (currentPrinterSupportsDisplayConfiguration() &&
                    (!displayState_.valid ||
                     displayState_.deviceSerial !=
                         printerDeviceSerial_.trimmed()) &&
                    displayStateReadGeneration_ != generation) {
                    displayStateReadGeneration_ = generation;
                    emit requestPrinterDisplayState(
                        currentPrinterPath(), printerGeneration_);
                }
                if (!activeOperationId_.isEmpty() &&
                    operations_.contains(activeOperationId_)) {
                    OperationRecord &record =
                        operations_[activeOperationId_];
                    if (record.uploadFinalizationReconciliationPending &&
                        record.info.stage ==
                            QStringLiteral("RecoveringFinalization")) {
                        if (!operationMatchesCurrentPrinterProduct(record)) {
                            const QString operationId =
                                activeOperationId_;
                            record.uploadFinalizationReconciliationPending =
                                false;
                            record.requiresDeviceRecovery = true;
                            record.retryMustUseNewRemoteName = true;
                            requirePrinterRecovery(tr(
                                "The reconnected USB product does not match the product that accepted the upload. Power-cycle the original device before a manual retry."));
                            handlePreparedUploadFailure(
                                operationId,
                                tr("The completed upload could not be reconciled safely because the USB product changed"),
                                PrinterProtocol::MutationOutcome::
                                    PartialOrUnknown);
                            emit printerOperationsCancelled();
                            return;
                        }
                        if (!currentPrinterSupportsMediaCatalog()) {
                            const QString operationId =
                                activeOperationId_;
                            record.uploadFinalizationReconciliationPending =
                                false;
                            record.requiresDeviceRecovery = true;
                            record.retryMustUseNewRemoteName = true;
                            requirePrinterRecovery(tr(
                                "This device has no supported media catalog, so an upload with a lost final acknowledgement cannot be reconciled safely. Power-cycle it before a manual retry."));
                            handlePreparedUploadFailure(
                                operationId,
                                tr("The upload outcome cannot be verified on this product"),
                                PrinterProtocol::MutationOutcome::
                                    PartialOrUnknown);
                            emit printerOperationsCancelled();
                            return;
                        }
                        const QString currentDeviceIdentity =
                            printerDeviceSerial_.trimmed();
                        if (record.uploadDeviceIdentity.isEmpty() ||
                            record.uploadDeviceIdentity !=
                                currentDeviceIdentity) {
                            const QString operationId =
                                activeOperationId_;
                            record.uploadFinalizationReconciliationPending =
                                false;
                            record.requiresDeviceRecovery = true;
                            record.retryMustUseNewRemoteName = true;
                            requirePrinterRecovery(tr(
                                "PASE reconnected with an unverified device identity after the final upload acknowledgement was lost. Power-cycle the device before a manual retry."));
                            handlePreparedUploadFailure(
                                operationId,
                                tr("The completed upload could not be reconciled safely because the USB device identity changed"),
                                PrinterProtocol::MutationOutcome::
                                    PartialOrUnknown);
                            emit printerOperationsCancelled();
                            return;
                        }
                        record.info.deviceGeneration =
                            printerGeneration_;
                        record.deviceChangePending = false;
                        record.deviceChangeMessage.clear();
                        record.info.state = QStringLiteral("Refreshing");
                        record.info.stage =
                            QStringLiteral("RefreshingMedia");
                        record.info.message = tr(
                            "The final upload acknowledgement was lost; verifying FileList without retransmitting media...");
                        publishOperation(activeOperationId_);
                        emit requestPrinterRefreshMedia(
                            currentPrinterPath(), activeOperationId_,
                            printerGeneration_);
                    }
                }
                const bool samplingActive =
                    currentPrinterSupportsOverlayMetrics() &&
                    metricsState_.enabled &&
                    !metricsState_.metrics.isEmpty();
                if (metricsState_.samplingActive != samplingActive) {
                    metricsState_.samplingActive = samplingActive;
                    publishMetricsState();
                }
                if (currentPrinterSupportsMediaCatalog()) {
                    resumePendingDeleteReconciliation();
                    resumePendingReplaceReconciliation();
                }
            });
    connect(worker_, &DeviceWorker::printerSessionStopped, this,
            [this](quint64 generation) {
                if (generation == printerGeneration_) {
                    setPrinterDisplaySessionActive(false);
                    printerSessionResumePending_ = false;
                    printerSessionResumeSerial_.clear();
                    printerSessionResumeProductId_ = 0;
                    if (metricsState_.samplingActive) {
                        metricsState_.samplingActive = false;
                        publishMetricsState();
                    }
                }
            });
    connect(worker_, &DeviceWorker::firmwareTransportQuiesced, this,
            [this](const QString &leaseId, quint64 generation) {
                if (leaseId != firmwareExclusiveLeaseId_ ||
                    generation != firmwareQuiesceGeneration_) {
                    return;
                }
                const bool wasConnected = connected_;
                detachPrinterClassDevice(false);
                legacyProductId_.clear();
                connected_ = false;
                setPrinterDisplaySessionActive(false);
                printerSessionResumePending_ = false;
                printerSessionResumeSerial_.clear();
                printerSessionResumeProductId_ = 0;
                emit mediaListUpdated({});
                if (wasConnected) {
                    emit deviceDisconnected();
                }
                emit firmwareTransportQuiesced(
                    leaseId, true,
                    tr("Device transports are closed for firmware flashing"));
            });
    connect(
        worker_,
        &DeviceWorker::firmwareQuiesceReleaseFenceReached,
        this,
        [this](const QString &leaseId, quint64 generation) {
            if (leaseId != firmwareExclusiveLeaseId_ ||
                leaseId != firmwareReleasePendingLeaseId_ ||
                generation != firmwareQuiesceGeneration_) {
                return;
            }
            const bool reconnect =
                firmwareReleaseResumeTransport_ ||
                firmwareRecoveryReconnectRequested_;
            firmwareExclusiveLeaseId_.clear();
            firmwareReleasePendingLeaseId_.clear();
            firmwareQuiesceGeneration_ = 0;
            firmwareResumeAutoConnect_ = false;
            firmwareReleaseResumeTransport_ = false;
            firmwareRecoveryReconnectRequested_ = false;
            if (reconnect) {
                connectDevice();
            } else {
                // A non-resuming release is a fail-closed recovery boundary,
                // not merely "do not reconnect right now". Disable passive
                // monitor-triggered reconnects until the user explicitly
                // starts a new connection after inspecting the device.
                autoConnectMode_ = false;
            }
        });
    connect(worker_, &DeviceWorker::printerSessionLost, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                clearDeviceSpecificationsCache();
                if (!activeOperationId_.isEmpty() &&
                    operations_.contains(activeOperationId_)) {
                    const QString operationId = activeOperationId_;
                    OperationRecord &record = operations_[operationId];
                    if (record.uploadFinalizationReconciliationPending) {
                        record.uploadFinalizationReconciliationPending =
                            false;
                        record.requiresDeviceRecovery = true;
                        record.retryMustUseNewRemoteName = true;
                        requirePrinterRecovery(tr(
                            "PASE did not recover far enough to verify the committed upload. Power-cycle the device before a manual retry."));
                        handlePreparedUploadFailure(
                            operationId,
                            tr("The final upload acknowledgement was lost and the read-only FileList reconciliation could not start"),
                            PrinterProtocol::MutationOutcome::PartialOrUnknown);
                        emit printerOperationsCancelled();
                        return;
                    }
                }
                if (!printerDisplaySessionLost_) {
                    printerSessionLossRemovalObserved_ = false;
                }
                printerDisplaySessionLost_ = true;
                setPrinterDisplaySessionActive(false);
                printerSessionResumePending_ = false;
                printerSessionResumeSerial_.clear();
                printerSessionResumeProductId_ = 0;
                if (metricsState_.samplingActive) {
                    metricsState_.samplingActive = false;
                    publishMetricsState();
                }
                cancelForegroundForGenerationChange(
                    tr("PASE display session was lost"));
                emit printerOperationsCancelled();
                emit uploadStatus(
                    tr("PASE display session is lost; waiting for a new USB endpoint generation"));
            });
    connect(worker_, &DeviceWorker::printerForegroundProgress, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &stage,
                                           qint64 completed, qint64 total,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.stage = stage;
                record.info.state =
                    stage == QStringLiteral("Beginning")
                        ? QStringLiteral("Beginning")
                        : stage == QStringLiteral("PullingDeviceMedia")
                            ? QStringLiteral("Pulling")
                        : stage == QStringLiteral("Transferring")
                            ? QStringLiteral("Transferring")
                            : stage == QStringLiteral("Ending")
                                ? QStringLiteral("Ending")
                                : stage == QStringLiteral("Applying")
                                    ? QStringLiteral("Applying")
                                    : QStringLiteral("Preflight");
                record.info.completed = completed;
                record.info.total = total;
                if (stage == QStringLiteral("Transferring") &&
                    completed >= record.info.confirmedBytes) {
                    record.info.confirmedBytes = completed;
                    record.info.lastConfirmedChunkIndex =
                        completed > 0
                            ? (completed - 1) / kFileTransmitChunkSize
                            : -1;
                } else if (
                    stage == QStringLiteral("PullingDeviceMedia") &&
                    completed > record.info.confirmedBytes) {
                    record.info.confirmedBytes = completed;
                    ++record.info.lastConfirmedChunkIndex;
                }
                record.info.message = message;
                publishOperation(operationId);
            });
    connect(worker_, &DeviceWorker::printerMediaStaged, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, const QString &mediaName,
                const QString &outputPath, bool success, bool cancelled,
                qint64 fileSize, qint64 chunkCount,
                const QString &rawSha256,
                const QString &decodedSha256,
                const RecoveredH264ProbeMetadata &probeMetadata,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(
                        operationId, generation)) {
                    const auto staleOperation =
                        operations_.constFind(operationId);
                    if (staleOperation != operations_.constEnd() &&
                        !staleOperation->artifactId.isEmpty()) {
                        const auto discarded =
                            deviceMediaArtifactStore_->discardReservation(
                                staleOperation->artifactId, operationId);
                        if (discarded.removed &&
                            artifactOwnerWatcher_ &&
                            !discarded.ownerUniqueName.isEmpty() &&
                            !deviceMediaArtifactStore_->ownerHasArtifacts(
                                discarded.ownerUniqueName)) {
                            artifactOwnerWatcher_->removeWatchedService(
                                discarded.ownerUniqueName);
                        }
                    }
                    return;
                }
                OperationRecord &record = operations_[operationId];
                const QString artifactId = record.artifactId;
                const auto artifact =
                    deviceMediaArtifactStore_->artifact(artifactId);
                if (!success || !artifact.ok() ||
                    artifact.artifact.canonicalPath != outputPath ||
                    artifact.artifact.metadata.remoteName != mediaName) {
                    const auto discarded =
                        deviceMediaArtifactStore_->discardReservation(
                            artifactId, operationId);
                    if (discarded.removed && artifactOwnerWatcher_ &&
                        !discarded.ownerUniqueName.isEmpty() &&
                        !deviceMediaArtifactStore_->ownerHasArtifacts(
                            discarded.ownerUniqueName)) {
                        artifactOwnerWatcher_->removeWatchedService(
                            discarded.ownerUniqueName);
                    }
                    finishOperation(
                        operationId,
                        cancelled ? QStringLiteral("Cancelled")
                                  : QStringLiteral("Failed"),
                        cancelled
                            ? QStringLiteral("UserCancelled")
                            : QStringLiteral("DeviceMediaPullFailed"),
                        QString(),
                        errorMessage.isEmpty()
                            ? tr("The device media copy could not be staged safely")
                            : errorMessage);
                    return;
                }
                tryx::DeviceMediaArtifactStore::FinalizeInput completion;
                completion.artifactId = artifactId;
                completion.operationId = operationId;
                completion.remoteName = mediaName;
                completion.outputPath = outputPath;
                completion.fileSize = fileSize;
                completion.chunkCount = chunkCount;
                completion.rawSha256 = rawSha256;
                completion.decodedSha256 = decodedSha256;
                completion.deviceGeneration = generation;
                if (probeMetadata.dimensionsAvailable) {
                    completion.width = probeMetadata.width;
                    completion.height = probeMetadata.height;
                }
                if (probeMetadata.frameCountAvailable) {
                    completion.frameCount = probeMetadata.frameCount;
                }
                const QString deviceIdentity =
                    printerDeviceSerial_.trimmed();
                const TryxRuntimeMediaEntry *currentEntry =
                    findMediaById(artifact.artifact.metadata.mediaId);
                if (currentEntry &&
                    deviceIdentity ==
                        artifact.artifact.metadata.deviceIdentity &&
                    record.uploadDeviceIdentity == deviceIdentity &&
                    record.uploadDeviceGeneration == generation &&
                    record.originalMediaId ==
                        artifact.artifact.metadata.mediaId) {
                    const auto remote =
                        mediaCatalogRemoteEntry(*currentEntry);
                    // The catalog lookup alone is insufficient because a
                    // same-name/same-size replacement keeps the same tuple.
                    // Bind the persisted origin to the exact artifact mediaId,
                    // then let finalize compare the pulled bytes hash.
                    const QString currentMediaId =
                        mediaCatalogStore_->mediaId(
                            deviceIdentity, remote);
                    if (currentMediaId ==
                            artifact.artifact.metadata.mediaId &&
                        currentEntry->name ==
                            artifact.artifact.metadata.remoteName &&
                        currentEntry->size ==
                            artifact.artifact.metadata.size &&
                        currentEntry->source == 1U &&
                        !currentEntry->readOnly) {
                        completion.managedOriginPreparedSha256 =
                            mediaCatalogStore_
                                ->managedOriginPreparedSha256(
                                    deviceIdentity, remote);
                    }
                }
                if (!watchArtifactOwner(record.artifactOwner)) {
                    handleArtifactOwnerUnregistered(
                        record.artifactOwner);
                }
                const auto finalized =
                    deviceMediaArtifactStore_->finalize(completion);
                if (!finalized.ok()) {
                    const auto discarded =
                        deviceMediaArtifactStore_->discardReservation(
                            artifactId, operationId);
                    if (discarded.removed && artifactOwnerWatcher_ &&
                        !discarded.ownerUniqueName.isEmpty() &&
                        !deviceMediaArtifactStore_->ownerHasArtifacts(
                            discarded.ownerUniqueName)) {
                        artifactOwnerWatcher_->removeWatchedService(
                            discarded.ownerUniqueName);
                    }
                    const bool ownerGone =
                        finalized.code ==
                            tryx::DeviceMediaArtifactStore::ErrorCode::Revoked ||
                        record.cancelRequested;
                    finishOperation(
                        operationId,
                        ownerGone ? QStringLiteral("Cancelled")
                                  : QStringLiteral("Failed"),
                        ownerGone
                            ? QStringLiteral("UserCancelled")
                            : QStringLiteral("ArtifactValidationFailed"),
                        QString(),
                        ownerGone
                            ? tr("Operation cancelled by the user")
                            : tr("The staged device media artifact failed its final identity check"));
                    return;
                }
                record.info.completed = fileSize;
                record.info.total = fileSize;
                record.info.confirmedBytes = fileSize;
                record.info.resultName = artifactId;
                qInfo().noquote()
                    << QStringLiteral(
                           "device_media_artifact=%1 operation=%2 bytes=%3 chunks=%4 raw_sha256=%5 decoded_sha256=%6")
                           .arg(artifactId, operationId)
                           .arg(fileSize)
                           .arg(chunkCount)
                           .arg(rawSha256, decodedSha256);
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QString(), QString(),
                    tr("Device media copy was staged and validated"));
            });
    connect(
        worker_, &DeviceWorker::printerReplacePreflightFinished,
        this,
        [this, printerOperationResultIsExpected](
            const QString &operationId, const QString &mediaName,
            const QString &expectedReplacementName,
            qint64 expectedReplacementSize,
            const QStringList &references,
            const QStringList &referencingSlots,
            const QString &activeScreenMode,
            const QString &activePlayMode,
            const QStringList &activeMedia,
            bool originalIdentityVerified,
            bool replacementIdentityVerified,
            bool success,
            const QString &errorMessage, quint64 generation) {
            if (!printerOperationResultIsExpected(
                    operationId, generation)) {
                return;
            }
            OperationRecord &record =
                operations_[operationId];
            if (!record.replaceOperation ||
                record.originalRemoteNameForReplace !=
                    mediaName) {
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("ReplacePreflightMismatch"),
                    QString(),
                    tr("The replace preflight returned a different media identity"));
                return;
            }
            const QString currentDeviceIdentity =
                printerDeviceSerial_.trimmed();
            if (generation != printerGeneration_ ||
                record.deviceChangePending ||
                currentDeviceIdentity.isEmpty() ||
                record.uploadDeviceIdentity.trimmed() !=
                    currentDeviceIdentity) {
                const bool mutationOutcomeUnknown =
                    record.replaceJournal.fileRemoveMayHaveStarted ||
                    (record.replaceJournal.applyMayHaveStarted &&
                     !record.replaceJournal.applyVerified);
                record.info.terminalOutcome =
                    mutationOutcomeUnknown
                    ? QStringLiteral("PartialOrUnknown")
                    : record.replaceJournal.uploadVerified
                        ? QStringLiteral("NewCopyReady")
                        : QStringLiteral("OriginalRetained");
                finishOperation(
                    operationId,
                    mutationOutcomeUnknown
                        ? QStringLiteral("RetryAvailable")
                        : record.replaceJournal.uploadVerified
                            ? QStringLiteral("Succeeded")
                            : QStringLiteral("Failed"),
                    mutationOutcomeUnknown
                        ? QStringLiteral("PartialOrUnknown")
                        : record.replaceJournal.uploadVerified
                            ? QStringLiteral("OriginalRetained")
                            : QStringLiteral("DeviceChanged"),
                    mutationOutcomeUnknown
                        ? QStringLiteral("ReconcileOnly")
                        : QString(),
                    record.deviceChangeMessage.isEmpty()
                        ? tr("The PASE connection changed before replacement preflight could be associated with the original device")
                        : record.deviceChangeMessage);
                return;
            }
            const bool reconcilingUnknownApply =
                record.info.stage ==
                QStringLiteral("ReconcilingUnknownApply");
            const bool reconcilingAfterApply =
                record.info.stage ==
                QStringLiteral("ReconcilingReferences");
            const bool replacementProofRequired =
                reconcilingUnknownApply ||
                reconcilingAfterApply;
            const bool replacementProofMatchesJournal =
                replacementProofRequired &&
                originalIdentityVerified &&
                replacementIdentityVerified &&
                expectedReplacementName ==
                    record.replaceJournal.newRemoteName &&
                expectedReplacementSize > 0 &&
                static_cast<quint64>(
                    expectedReplacementSize) ==
                    record.replaceJournal.newSize;
            if (replacementProofRequired &&
                !replacementProofMatchesJournal) {
                record.info.terminalOutcome =
                    QStringLiteral("PartialOrUnknown");
                record.info.resultName =
                    record.replaceJournal.newRemoteName;
                finishOperation(
                    operationId,
                    QStringLiteral("RetryAvailable"),
                    QStringLiteral("PartialOrUnknown"),
                    QStringLiteral("ReconcileOnly"),
                    tr("The fresh FileList did not prove the exact original and replacement identities. Replace remains unresolved and no mutation was repeated."));
                return;
            }
            if (success && !originalIdentityVerified) {
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("ReplacePreflightInvalid"),
                    QString(),
                    tr("The replace preflight succeeded without proving the original media identity"));
                return;
            }
            if (!success) {
                if (reconcilingAfterApply) {
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        errorMessage.isEmpty()
                            ? tr("The new copy is active, but the original was retained because its references could not be re-read")
                            : tr("The new copy is active, but the original was retained: %1")
                                  .arg(errorMessage));
                    return;
                }
                if (reconcilingUnknownApply) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("ReconcileOnly"),
                        errorMessage.isEmpty()
                            ? tr("The previous Apply outcome is still unknown; no mutation was repeated")
                            : tr("The previous Apply outcome is still unknown: %1")
                                  .arg(errorMessage));
                    return;
                }
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("ReplacePreflightFailed"),
                    QString(),
                    errorMessage.isEmpty()
                        ? tr("The original media references could not be verified")
                        : errorMessage);
                return;
            }
            const QStringList expectedSlots{
                QStringLiteral("PowerOn"),
                QStringLiteral("Standby"),
                QStringLiteral("Single"),
                QStringLiteral("DualLeft"),
                QStringLiteral("DualRight"),
                QStringLiteral("Kaleidoscope"),
                QStringLiteral("FilterSingle"),
                QStringLiteral("FilterDualLeft"),
                QStringLiteral("FilterDualRight"),
            };
            if (references.size() != expectedSlots.size()) {
                finishOperation(
                    operationId,
                    reconcilingUnknownApply
                        ? QStringLiteral("RetryAvailable")
                        : reconcilingAfterApply
                            ? QStringLiteral("Succeeded")
                            : QStringLiteral("Failed"),
                    reconcilingUnknownApply
                        ? QStringLiteral("PartialOrUnknown")
                        : reconcilingAfterApply
                            ? QStringLiteral("OriginalRetained")
                            : QStringLiteral(
                                  "ReplacePreflightInvalid"),
                    reconcilingUnknownApply
                        ? QStringLiteral("ReconcileOnly")
                        : QString(),
                    tr("The device returned an incomplete media reference set"));
                return;
            }
            if (!replacementProofRequired &&
                (activeScreenMode !=
                     record.applyRequest.screenMode ||
                 activePlayMode !=
                     record.applyRequest.playMode ||
                 activeMedia != record.applyRequest.media)) {
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("ReplaceLayoutChanged"),
                    QString(),
                    tr("The active device layout changed during replacement preflight; reopen the editor and try again"));
                return;
            }
            if (reconcilingUnknownApply) {
                record.replaceReferences = references;
                record.replaceReferenceSlots =
                    referencingSlots;
                record.replaceJournal.disposition =
                    QStringLiteral("NewCopyReady");
                QString journalError;
                if (!writeReplaceJournal(
                        operationId,
                        QStringLiteral("ReferenceReconciliation"),
                        &journalError)) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("ReconcileOnly"),
                        tr("Apply was not repeated, but the read-only reconciliation could not be persisted: %1")
                            .arg(journalError));
                    return;
                }
                record.info.terminalOutcome =
                    QStringLiteral("NewCopyReady");
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QStringLiteral("OriginalRetained"),
                    QString(),
                    referencingSlots.isEmpty()
                        ? tr("The previous Apply was not repeated and its outcome remains unknown. The new copy is ready and the original was retained.")
                        : tr("The previous Apply was not repeated and its outcome remains unknown. The original is still referenced by: %1")
                              .arg(referencingSlots.join(
                                  QStringLiteral(", "))));
                return;
            }
            if (reconcilingAfterApply) {
                record.replaceReferences = references;
                record.replaceReferenceSlots =
                    referencingSlots;
                if (!referencingSlots.isEmpty()) {
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The new copy is active, but the original media is still referenced by: %1")
                            .arg(referencingSlots.join(
                                QStringLiteral(", "))));
                    return;
                }

                QString journalError;
                if (!writeReplaceJournal(
                        operationId,
                        QStringLiteral("ReferenceReconciliation"),
                        &journalError)) {
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The new copy is active, but the original was retained because replace state could not be persisted: %1")
                            .arg(journalError));
                    return;
                }

                record.deleteNames = {
                    record.originalRemoteNameForReplace};
                if (!writeDeleteIntent(
                        operationId,
                        QStringLiteral("Preflight"),
                        false, 0,
                        record.originalRemoteNameForReplace,
                        {}, &journalError)) {
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The new copy is active, but the original was retained because delete intent could not be persisted: %1")
                            .arg(journalError));
                    return;
                }

                record.replaceJournal.deleteIntentLinked = true;
                if (!writeReplaceJournal(
                        operationId,
                        QStringLiteral("DeleteIntentLinked"),
                        &journalError)) {
                    QString clearError;
                    clearDeleteIntent(operationId, &clearError);
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The new copy is active, but the original was retained because the replace/delete link could not be persisted: %1")
                            .arg(journalError));
                    return;
                }

                record.replaceJournal.fileRemoveMayHaveStarted = true;
                if (!writeReplaceJournal(
                        operationId,
                        QStringLiteral("Deleting"),
                        &journalError)) {
                    QString clearError;
                    clearDeleteIntent(operationId, &clearError);
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The new copy is active, but the original was retained because the delete boundary could not be persisted: %1")
                            .arg(journalError));
                    return;
                }

                record.info.state = QStringLiteral("Deleting");
                record.info.stage = QStringLiteral("DeletePreflight");
                record.info.message = tr(
                    "No references to the original remain; deleting it once...");
                publishOperation(operationId);
                emit requestPrinterDeleteMedia(
                    currentPrinterPath(), record.deleteNames,
                    operationId, deleteIntentPath(), false,
                    static_cast<qint64>(
                        record.replaceJournal.originalSize),
                    record.replaceJournal.newRemoteName,
                    static_cast<qint64>(
                        record.replaceJournal.newSize),
                    printerGeneration_);
                return;
            }
            const bool splitScreen =
                record.applyRequest.screenMode ==
                QStringLiteral("Screen Splitting");
            const QSet<QString> replaceableSlots = splitScreen
                ? QSet<QString>{
                      QStringLiteral("DualLeft"),
                      QStringLiteral("DualRight")}
                : QSet<QString>{QStringLiteral("Single")};
            QStringList blockedSlots;
            for (const QString &slot : referencingSlots) {
                if (!replaceableSlots.contains(slot)) {
                    blockedSlots.append(slot);
                }
            }
            if (referencingSlots.isEmpty() ||
                !blockedSlots.isEmpty()) {
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("OriginalStillReferenced"),
                    QString(),
                    referencingSlots.isEmpty()
                        ? tr("The original media is not referenced by the active layout; use Save as new instead")
                        : tr("Replace is blocked because the original media is also referenced by: %1")
                              .arg(blockedSlots.join(
                                  QStringLiteral(", "))));
                return;
            }
            record.replaceReferences = references;
            record.replaceReferenceSlots =
                referencingSlots;
            QSet<QString> seenReferences;
            QStringList uniqueReferences;
            for (const QString &reference : references) {
                if (!reference.isEmpty() &&
                    !seenReferences.contains(reference)) {
                    seenReferences.insert(reference);
                    uniqueReferences.append(reference);
                }
            }
            record.replaceJournal.operationId = operationId;
            record.replaceJournal.productId =
                record.printerProductId;
            record.replaceJournal.deviceIdentity =
                record.uploadDeviceIdentity;
            record.replaceJournal.deviceGeneration =
                record.uploadDeviceGeneration;
            record.replaceJournal.originalMediaId =
                record.originalMediaId;
            record.replaceJournal.originalRemoteName =
                record.originalRemoteNameForReplace;
            record.replaceJournal.originalSize =
                static_cast<quint64>(record.sourceSize);
            record.replaceJournal.artifactId =
                record.artifactId;
            record.replaceJournal.decodedSha256 =
                record.sourceContentSha256;
            record.replaceJournal.transformFingerprint =
                tryxMediaPreparationProfileFingerprint(
                    record.mediaPreparationProfile);
            record.replaceJournal.applyFingerprint =
                runtimeApplyRequestFingerprint(
                    record.applyRequest);
            record.replaceJournal.referenceNames =
                uniqueReferences;
            record.replaceJournalActive = true;
            QString journalError;
            if (!writeReplaceJournal(
                    operationId, QStringLiteral("Preflight"),
                    &journalError) ||
                !writeReplaceJournal(
                    operationId, QStringLiteral("Preparing"),
                    &journalError)) {
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("ReplaceJournalWriteFailed"),
                    QString(),
                    tr("Replacement was stopped before upload because its journal could not be persisted: %1")
                        .arg(journalError));
                return;
            }
            record.info.state =
                QStringLiteral("Converting");
            record.info.stage =
                QStringLiteral("Converting");
            record.info.message =
                tr("Reference preflight passed; preparing the replacement media...");
            publishOperation(operationId);
            emit requestEndPrinterForegroundOperation(
                operationId, generation);
            if (record.mediaPreparationProfile.target ==
                QStringLiteral("SplitArea")) {
                emit requestPrepareRecoveredPrinterMediaWithPreparationProfile(
                    operationId, currentPrinterPath(),
                    record.sourcePath,
                    record.sourceContentSha256, generation,
                    record.mediaPreparationProfile,
                    record.printerProductId);
            } else {
                emit requestPrepareRecoveredPrinterMedia(
                    operationId, currentPrinterPath(),
                    record.sourcePath,
                    record.sourceContentSha256, generation,
                    record.mediaTransform,
                    record.printerProductId);
            }
        });
    connect(worker_, &DeviceWorker::printerUploadFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, const QString &uploadPath,
                const QString &remoteName, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.preparedPath = uploadPath;
                record.remoteName = remoteName;
                if (record.originalRemoteName.isEmpty()) {
                    record.originalRemoteName = remoteName;
                }
                record.info.resultName = remoteName;
                if (!success) {
                    const QString outcomeName = mutationOutcomeName(outcome);
                    if (record.info.terminalOutcome.isEmpty()) {
                        record.info.terminalOutcome = outcomeName;
                    }
                    if (record.info.primaryErrorCategory.isEmpty()) {
                        record.info.primaryErrorCategory = outcomeName;
                    }
                    if (record.info.primaryErrorMessage.isEmpty()) {
                        record.info.primaryErrorMessage = errorMessage;
                    }
                    if (outcome ==
                        PrinterProtocol::MutationOutcome::
                            FinalizationUnknown) {
                        if (!currentPrinterSupportsMediaCatalog()) {
                            record.uploadFinalizationReconciliationPending =
                                false;
                            record.info.terminalOutcome =
                                QStringLiteral("PartialOrUnknown");
                            handlePreparedUploadFailure(
                                operationId,
                                tr("The upload outcome cannot be verified on this product"),
                                PrinterProtocol::MutationOutcome::
                                    PartialOrUnknown);
                            return;
                        }
                        record.info.confirmedBytes =
                            qMax(record.info.confirmedBytes,
                                 record.info.total);
                        record.info.lastConfirmedChunkIndex =
                            record.info.confirmedBytes > 0
                                ? (record.info.confirmedBytes - 1) /
                                      kFileTransmitChunkSize
                                : -1;
                        if (record.uploadDeviceIdentity.isEmpty()) {
                            record.uploadDeviceIdentity =
                                printerDeviceSerial_.trimmed();
                        }
                        record.uploadFinalizationReconciliationPending =
                            true;
                        handlePreparedUploadFailure(
                            operationId, errorMessage, outcome);
                        return;
                    }
                    handlePreparedUploadFailure(operationId, errorMessage,
                                                outcome);
                    return;
                }
                if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
                    retryCacheSnapshot_.inFlightDispatch->operationId !=
                        operationId ||
                    retryCacheSnapshot_.inFlightDispatch->phase !=
                        tryx::RetryCacheStore::DispatchPhase::
                            DispatchArmed ||
                    retryCacheSnapshot_.inFlightDispatch
                            ->deviceGeneration != generation ||
                    retryCacheSnapshot_.inFlightDispatch
                            ->retryRemoteName != remoteName ||
                    retryCacheArtifactPath(
                        retryCacheSnapshot_.inFlightDispatch->prepared) !=
                        QFileInfo(uploadPath).absoluteFilePath()) {
                    retryCacheStartupFailure_ = true;
                    retryCacheFailureDetail_ = tr(
                        "The acknowledged upload did not match the durable dispatch identity");
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("RetryCacheConflict"), QString(),
                        retryCacheFailureDetail_);
                    return;
                }
                auto updated = operations_.find(operationId);
                if (updated == operations_.end()) {
                    return;
                }
                if (updated->cancelRequested) {
                    worker_->clearPrinterOperationCancellation(operationId);
                }
                if (updated->deviceChangePending) {
                    handlePreparedUploadFailure(
                        operationId,
                        updated->deviceChangeMessage.isEmpty()
                            ? tr("The upload succeeded, but USB changed before local verification could finish")
                            : updated->deviceChangeMessage,
                        PrinterProtocol::MutationOutcome::
                            PartialOrUnknown);
                    return;
                }
                if (!currentPrinterSupportsMediaCatalog()) {
                    QString retirementError;
                    if (!retireRetryCacheDispatch(
                            operationId,
                            tryx::RetryCacheStore::
                                DispatchRetirement::
                                    AcknowledgedSuccess,
                            &retirementError)) {
                        const bool durableCleanupPending =
                            retryCacheDispatchRetiredIntoCleanup(
                                retryCacheSnapshot_);
                        if (durableCleanupPending) {
                            const QString completedRemoteName =
                                updated->remoteName;
                            emit mediaUploaded(completedRemoteName);
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"),
                                QStringLiteral(
                                    "RetryCacheCleanupFailed"),
                                QString(),
                                retirementError.isEmpty()
                                    ? tr("Media uploaded and activated; local retry cleanup remains pending and device mutations are blocked")
                                    : tr("Media uploaded and activated; local retry cleanup remains pending: %1")
                                          .arg(retirementError));
                            return;
                        }
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral(
                                "RetryCacheRetirementFailed"),
                            QString(), retirementError.isEmpty()
                                ? tr("The upload succeeded remotely, but its durable retry record could not be retired")
                                : retirementError);
                        return;
                    }
                    updated = operations_.find(operationId);
                    if (updated == operations_.end()) {
                        return;
                    }
                    if (updated->info.applyAfterUpload) {
                        removePreparedFileForOperation(operationId);
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("UnsupportedProduct"),
                            QString(), tr(
                                "Media was uploaded, but display configuration is not supported for this product"));
                        return;
                    }
                    const QString completedRemoteName =
                        updated->remoteName;
                    const bool cancellationRequested =
                        updated->cancelRequested;
                    removePreparedFileForOperation(operationId);
                    emit mediaUploaded(completedRemoteName);
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        cancellationRequested
                            ? tr("Media was uploaded and activated before cancellation completed")
                            : tr("Media uploaded and activated"));
                    return;
                }
                updated->info.state = QStringLiteral("Refreshing");
                updated->info.stage = QStringLiteral("RefreshingMedia");
                updated->info.message =
                    tr("Upload acknowledged; verifying the device file list...");
                publishOperation(operationId);
                emit requestPrinterRefreshMedia(currentPrinterPath(),
                                                operationId,
                                                printerGeneration_);
            });
    connect(worker_, &DeviceWorker::printerMediaListReady, this,
            [this, printerResultIsCurrent,
             printerOperationResultIsExpected](const QString &operationId,
                                           const QList<PrinterProtocol::MediaFile> &mediaFiles,
                                           quint64 generation) {
                QStringList files;
                QSet<QString> seenNames;
                for (const PrinterProtocol::MediaFile &media : mediaFiles) {
                    if (!seenNames.contains(media.name)) {
                        seenNames.insert(media.name);
                        files.append(media.name);
                    }
                }
                if (operationId.isEmpty()) {
                    if (printerResultIsCurrent(generation)) {
                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                    }
                    return;
                }
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (!printerResultIsCurrent(generation)) {
                    handlePreparedUploadFailure(
                        operationId,
                        record.deviceChangeMessage.isEmpty()
                            ? tr("USB changed before FileList could be reconciled")
                            : record.deviceChangeMessage,
                        record.retryPreflight
                            ? PrinterProtocol::MutationOutcome::NotStarted
                            : PrinterProtocol::MutationOutcome::PartialOrUnknown);
                    return;
                }
                if (record.originLookupPending) {
                    record.originLookupPending = false;
                    updateMediaCatalog(mediaFiles);
                    emit mediaListUpdated(files);
                    if (record.cancelRequested) {
                        finishOperation(
                            operationId, QStringLiteral("Cancelled"),
                            QStringLiteral("UserCancelled"), QString(),
                            tr("Operation cancelled by the user"));
                        return;
                    }
                    const QString reusableName = findReusableMediaOrigin(
                        record.sourceContentSha256,
                        record.conversionProfile, mediaFiles);
                    if (!reusableName.isEmpty()) {
                        record.remoteName = reusableName;
                        record.mediaFile = reusableName;
                        releaseOwnedSource(record);
                        record.info.resultName = reusableName;
                        record.info.state = QStringLiteral("Applying");
                        record.info.stage = QStringLiteral("ReusingExisting");
                        record.info.message = tr(
                            "The media is already on the device; applying it without conversion or upload...");
                        publishOperation(operationId);
                        emit requestPrinterApplyMedia(
                            currentPrinterPath(), record.mediaFile,
                            record.applyRequest, record.updateMetrics,
                            QString(), {},
                            operationId, printerGeneration_);
                        return;
                    }
                    record.info.state = QStringLiteral("Converting");
                    record.info.stage = QStringLiteral("Converting");
                    record.info.message = tr(
                        "No confirmed existing copy was found; preparing media for upload...");
                    publishOperation(operationId);
                    emit requestEndPrinterForegroundOperation(operationId,
                                                              generation);
                    if (record.mediaPreparationProfile.target ==
                        QStringLiteral("SplitArea")) {
                        emit requestPreparePrinterMediaWithPreparationProfile(
                            operationId, currentPrinterPath(),
                            record.sourcePath,
                            record.sourceContentSha256,
                            printerGeneration_,
                            record.mediaPreparationProfile,
                            record.printerProductId);
                    } else {
                        emit requestPreparePrinterMedia(
                            operationId, currentPrinterPath(),
                            record.sourcePath,
                            record.sourceContentSha256,
                            printerGeneration_, record.mediaTransform,
                            record.printerProductId);
                    }
                    return;
                }
                const bool finalizationCandidate =
                    retryCacheSnapshot_.retryCandidate.has_value() &&
                    retryCacheSnapshot_.retryCandidate->operationId ==
                        operationId &&
                    retryCacheSnapshot_.retryCandidate
                        ->finalizationOnlyReconciliation;
                const bool shadowMissingFence =
                    retryCacheSnapshot_.inFlightDispatch.has_value() &&
                    retryCacheSnapshot_.inFlightDispatch->operationId ==
                        operationId &&
                    retryCacheDispatchPhaseIsRestricted(
                        retryCacheSnapshot_.inFlightDispatch->phase);
                const qint64 durablePreparedSize = finalizationCandidate
                    ? retryCacheSnapshot_.retryCandidate->prepared.size
                    : shadowMissingFence
                        ? retryCacheSnapshot_.inFlightDispatch
                              ->prepared.size
                        : record.info.total;
                const QFileInfo preparedInfo(record.preparedPath);
                const qint64 preparedSize = durablePreparedSize > 0
                    ? durablePreparedSize
                    : preparedInfo.size();
                qsizetype matchingRemoteNameCount = 0;
                auto exactMedia = mediaFiles.cend();
                for (auto media = mediaFiles.cbegin();
                     media != mediaFiles.cend(); ++media) {
                    if (media->name != record.remoteName) {
                        continue;
                    }
                    ++matchingRemoteNameCount;
                    if (media->source ==
                            PrinterProtocol::MediaSource::User &&
                        !media->readOnly &&
                        static_cast<qint64>(media->size) == preparedSize) {
                        exactMedia = media;
                    }
                }
                const bool exactPreparedFilePresent =
                    preparedSize > 0 && matchingRemoteNameCount == 1 &&
                    exactMedia != mediaFiles.cend();
                const auto persistVerifiedReplaceUploadBeforeRetirement =
                    [this, exactMedia, operationId]() {
                        auto operation = operations_.find(operationId);
                        if (operation == operations_.end() ||
                            !operation->replaceOperation) {
                            return true;
                        }
                        operation->replaceJournal.newRemoteName =
                            exactMedia->name;
                        operation->replaceJournal.newSize =
                            exactMedia->size;
                        operation->replaceJournal.uploadVerified = true;
                        operation->replaceJournal.disposition =
                            QStringLiteral("NewCopyReady");
                        QString journalError;
                        if (writeReplaceJournal(
                                operationId,
                                QStringLiteral("UploadVerified"),
                                &journalError)) {
                            return true;
                        }
                        operation = operations_.find(operationId);
                        if (operation != operations_.end()) {
                            operation->info.terminalOutcome =
                                QStringLiteral("NewCopyReady");
                        }
                        retryCacheStartupFailure_ = true;
                        retryCacheFailureDetail_ = journalError.isEmpty()
                            ? tr("The verified replacement could not be recorded durably")
                            : journalError;
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("PersistenceFailed"),
                            QString(),
                            tr("The new copy is verified, but Apply was not started because replace state could not be persisted. Restart is required before further device mutations: %1")
                                .arg(retryCacheFailureDetail_),
                            true);
                        return false;
                    };
                const auto finishVerifiedUploadAfterRetirement =
                    [this, &files, &mediaFiles, preparedSize,
                     operationId](bool retirementCleanupPending,
                                  bool restrictedProof) {
                        auto completed = operations_.find(operationId);
                        if (completed == operations_.end()) {
                            return;
                        }
                        completed->info.confirmedBytes = preparedSize;
                        completed->info.lastConfirmedChunkIndex =
                            preparedSize > 0
                            ? (preparedSize - 1) /
                                  kFileTransmitChunkSize
                            : -1;

                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                        if (!retirementCleanupPending) {
                            removePreparedFileForOperation(operationId);
                        }
                        completed = operations_.find(operationId);
                        if (completed == operations_.end()) {
                            return;
                        }
                        emit mediaUploaded(completed->remoteName);

                        if (retirementCleanupPending) {
                            const bool followUpBlocked =
                                !completed->cancelRequested &&
                                (completed->info.applyAfterUpload ||
                                 completed->replaceOperation);
                            if (completed->replaceOperation) {
                                completed->info.terminalOutcome =
                                    QStringLiteral("NewCopyReady");
                            }
                            finishOperation(
                                operationId,
                                followUpBlocked
                                    ? QStringLiteral("Failed")
                                    : QStringLiteral("Succeeded"),
                                QStringLiteral(
                                    "RetryCacheCleanupFailed"),
                                QString(),
                                followUpBlocked
                                    ? tr("The upload is verified, but Apply or Replace is blocked until local retry cleanup completes; the uploaded copy remains on the device")
                                    : tr("The upload is verified; local retry cleanup remains pending and device mutations are blocked"),
                                completed->replaceOperation);
                            return;
                        }

                        if (restrictedProof) {
                            promoteRestrictedSessionAfterProof();
                            completed = operations_.find(operationId);
                            if (completed == operations_.end()) {
                                return;
                            }
                        }
                        if (completed->cancelRequested) {
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"), QString(),
                                QString(),
                                completed->info.applyAfterUpload
                                    ? tr("Upload completed before cancellation; apply was skipped")
                                    : tr("Upload completed before cancellation"));
                            return;
                        }
                        if (completed->info.applyAfterUpload) {
                            completed->mediaFile = completed->remoteName;
                            if (completed->replaceOperation) {
                                bool replacedReference = false;
                                for (QString &media :
                                     completed->applyRequest.media) {
                                    if (media == completed
                                                     ->originalRemoteNameForReplace) {
                                        media = completed->remoteName;
                                        replacedReference = true;
                                    }
                                }
                                if (!replacedReference) {
                                    completed->info.terminalOutcome =
                                        QStringLiteral("NewCopyReady");
                                    finishOperation(
                                        operationId,
                                        QStringLiteral("Succeeded"),
                                        QStringLiteral(
                                            "OriginalRetained"),
                                        QString(),
                                        tr("The new copy is ready, but the explicit layout no longer references the original media"));
                                    return;
                                }
                                completed->info.terminalOutcome =
                                    QStringLiteral("NewCopyReady");
                                completed->replaceJournal
                                    .applyMayHaveStarted = true;
                                QString journalError;
                                if (!writeReplaceJournal(
                                        operationId,
                                        QStringLiteral("Applying"),
                                        &journalError)) {
                                    finishOperation(
                                        operationId,
                                        QStringLiteral("Succeeded"),
                                        QStringLiteral(
                                            "OriginalRetained"),
                                        QString(),
                                        tr("The new copy is ready, but Apply was not started because replace state could not be persisted: %1")
                                            .arg(journalError));
                                    return;
                                }
                            }
                            completed->info.state =
                                QStringLiteral("Applying");
                            completed->info.stage =
                                QStringLiteral("Applying");
                            completed->info.message =
                                tr("Applying the verified media...");
                            publishOperation(operationId);
                            emit requestPrinterApplyMedia(
                                currentPrinterPath(),
                                completed->mediaFile,
                                completed->applyRequest,
                                completed->updateMetrics, QString(), {},
                                operationId,
                                printerGeneration_);
                            return;
                        }
                        finishOperation(
                            operationId, QStringLiteral("Succeeded"),
                            QString(), QString(),
                            restrictedProof
                                ? tr("The previous upload was confirmed by FileList and its retry record was retired")
                                : tr("Media uploaded and verified"));
                    };
                if (finalizationCandidate || shadowMissingFence) {
                    if (!exactPreparedFilePresent) {
                        if (finalizationCandidate) {
                            const auto candidate =
                                *retryCacheSnapshot_.retryCandidate;
                            const auto resolved = retryCacheStore()
                                .resolveCandidateRecovery(
                                    retryCacheSnapshot_,
                                    retryCacheExpectedDispatch(candidate),
                                    tryx::RetryCacheStore::
                                        CandidateRecoveryProof::
                                            FinalizationNotFound);
                            if (!resolved.ok() ||
                                !resolved.snapshot.has_value()) {
                                retryCacheStartupFailure_ = true;
                                retryCacheFailureDetail_ =
                                    resolved.detail;
                                finishOperation(
                                    operationId,
                                    QStringLiteral("Failed"),
                                    QStringLiteral(
                                        "RetryCacheRecoveryFailed"),
                                    QString(),
                                    resolved.detail.isEmpty()
                                        ? tr("The missing final upload could not be recorded durably")
                                        : resolved.detail);
                                return;
                            }
                            retryCacheSnapshot_ = *resolved.snapshot;
                            synchronizeRetryCacheSurface();
                            auto recovered = operations_.find(operationId);
                            if (recovered != operations_.end()) {
                                recovered->info.terminalOutcome =
                                    QStringLiteral("PartialOrUnknown");
                                recovered->requiresDeviceRecovery = true;
                                recovered->retryMustUseNewRemoteName = true;
                                recovered
                                    ->uploadFinalizationReconciliationPending =
                                    false;
                            }
                            const QString recoveryMessage = tr(
                                "FileList did not confirm the final upload. Power-cycle PASE before retrying the preserved media under a new name.");
                            finishOperation(
                                operationId,
                                QStringLiteral("RetryAvailable"),
                                QStringLiteral("PartialOrUnknown"),
                                QStringLiteral("PreparedMedia"),
                                recoveryMessage);
                            requirePrinterRecovery(recoveryMessage);
                        } else {
                            const QString recoveryMessage = tr(
                                "FileList did not prove the fenced upload outcome. Physically reconnect the same PASE before device mutations resume.");
                            pauseOperationForRetryCacheReconciliation(
                                operationId,
                                QStringLiteral("ShadowMissingFence"),
                                recoveryMessage);
                            requirePrinterRecovery(recoveryMessage);
                        }
                        return;
                    }

                    TryxRuntimeMediaEntry verifiedEntry;
                    verifiedEntry.name = exactMedia->name;
                    verifiedEntry.size = exactMedia->size;
                    verifiedEntry.source = 1U;
                    verifiedEntry.readOnly = exactMedia->readOnly;
                    if (!persistVerifiedReplaceUploadBeforeRetirement()) {
                        return;
                    }
                    const bool localMetadataRequired =
                        !record.stagedThumbnailPath.isEmpty() ||
                        record.ensureExisting ||
                        isSha256Hex(record.sourceContentSha256) ||
                        record.replaceOperation;
                    bool retirementCleanupPending = false;
                    if (localMetadataRequired) {
                        QString storeError;
                        if (!beginRetryCacheLocalCommit(
                                operationId, verifiedEntry,
                                &storeError)) {
                            finishOperation(
                                operationId,
                                QStringLiteral("Failed"),
                                QStringLiteral(
                                    "RetryCacheLocalCommitFailed"),
                                QString(), storeError);
                            return;
                        }
                        QString localErrorCategory;
                        QString localErrorMessage;
                        if (!commitVerifiedMediaMetadata(
                                operationId, verifiedEntry,
                                &localErrorCategory,
                                &localErrorMessage)) {
                            QString deferError;
                            if (!deferRetryCacheLocalCommit(
                                    operationId,
                                    localErrorCategory,
                                    localErrorMessage,
                                    &deferError)) {
                                finishOperation(
                                    operationId,
                                    QStringLiteral("Failed"),
                                    QStringLiteral(
                                        "RetryCacheLocalCommitFailed"),
                                    QString(),
                                    deferError.isEmpty()
                                        ? localErrorMessage
                                        : deferError);
                                return;
                            }
                            auto deferred =
                                operations_.find(operationId);
                            if (deferred != operations_.end()) {
                                deferred->info.terminalOutcome =
                                    QStringLiteral("NotStarted");
                                deferred->requiresDeviceRecovery = false;
                                deferred->retryMustUseNewRemoteName = false;
                                deferred
                                    ->uploadFinalizationReconciliationPending =
                                    false;
                                if (deferred->info
                                        .primaryErrorCategory
                                        .isEmpty()) {
                                    deferred->info.primaryErrorCategory =
                                        localErrorCategory;
                                    deferred->info.primaryErrorMessage =
                                        localErrorMessage;
                                }
                            }
                            updateMediaCatalog(mediaFiles);
                            emit mediaListUpdated(files);
                            finishOperation(
                                operationId,
                                QStringLiteral("RetryAvailable"),
                                localErrorCategory,
                                QStringLiteral("PreparedMedia"),
                                localErrorMessage);
                            promoteRestrictedSessionAfterProof();
                            return;
                        }
                        if (!retireRetryCacheDispatch(
                                operationId,
                                tryx::RetryCacheStore::
                                    DispatchRetirement::
                                        AcknowledgedSuccess,
                                &storeError)) {
                            retirementCleanupPending =
                                retryCacheDispatchRetiredIntoCleanup(
                                    retryCacheSnapshot_);
                            if (!retirementCleanupPending) {
                                finishOperation(
                                    operationId,
                                    QStringLiteral("Failed"),
                                    QStringLiteral(
                                        "RetryCacheRetirementFailed"),
                                    QString(), storeError);
                                return;
                            }
                        }
                    } else {
                        tryx::RetryCacheStore::MutationResult resolved;
                        if (finalizationCandidate) {
                            const auto candidate =
                                *retryCacheSnapshot_.retryCandidate;
                            resolved = retryCacheStore()
                                .consumeCandidate(
                                    retryCacheSnapshot_,
                                    retryCacheExpectedDispatch(
                                        candidate));
                        } else {
                            const auto dispatch =
                                *retryCacheSnapshot_.inFlightDispatch;
                            if (dispatch.phase !=
                                tryx::RetryCacheStore::DispatchPhase::
                                    ShadowMissingFence) {
                                pauseOperationForRetryCacheReconciliation(
                                    operationId,
                                    QStringLiteral(
                                        "ShadowMissingFence"),
                                    tr("The physical reconnect fence is still pending and cannot be resolved by FileList"));
                                return;
                            }
                            resolved = retryCacheStore()
                                .resolveShadowMissingFence(
                                    retryCacheSnapshot_,
                                    retryCacheExpectedDispatch(
                                        dispatch),
                                    tryx::RetryCacheStore::
                                        RecoveryFenceProof::
                                            ReadOnlyConfirmedSuccess);
                        }
                        if (!resolved.ok() ||
                            !resolved.snapshot.has_value()) {
                            retirementCleanupPending =
                                resolved.snapshot.has_value() &&
                                retryCacheDispatchRetiredIntoCleanup(
                                    *resolved.snapshot);
                            if (!retirementCleanupPending) {
                                retryCacheStartupFailure_ = true;
                                retryCacheFailureDetail_ =
                                    resolved.detail;
                                finishOperation(
                                    operationId,
                                    QStringLiteral("Failed"),
                                    QStringLiteral(
                                        "RetryCacheRecoveryFailed"),
                                    QString(), resolved.detail.isEmpty()
                                        ? tr("The read-only upload proof could not retire the retry record")
                                        : resolved.detail);
                                return;
                            }
                            retryCacheSnapshot_ = *resolved.snapshot;
                            retryCacheStartupFailure_ = true;
                            retryCacheFailureDetail_ =
                                resolved.detail.isEmpty()
                                ? tr("The retry record was retired, but local cleanup remains pending")
                                : resolved.detail;
                        } else {
                            retryCacheSnapshot_ = *resolved.snapshot;
                        }
                        synchronizeRetryCacheSurface();
                    }
                    auto reconciled = operations_.find(operationId);
                    if (reconciled == operations_.end()) {
                        return;
                    }
                    reconciled->uploadFinalizationReconciliationPending =
                        false;
                    finishVerifiedUploadAfterRetirement(
                        retirementCleanupPending, true);
                    return;
                }
                const bool retryRequiresFreshTarget =
                    record.retryPreflight &&
                    record.retryMustUseNewRemoteName;
                const bool exactPreparedFileCanBeTrusted =
                    exactPreparedFilePresent &&
                    !retryRequiresFreshTarget;
                if (record.retryPreflight) {
                    record.retryPreflight = false;
                    if (record.cancelRequested) {
                        handlePreparedUploadFailure(
                            operationId,
                            tr("Operation cancelled by the user"),
                            PrinterProtocol::MutationOutcome::Cancelled);
                        return;
                    }
                    if (record.retryMustUseNewRemoteName) {
                        const QString previousRemoteName =
                            record.remoteName;
                        const QString nameForSuffix =
                            record.originalRemoteName.isEmpty()
                                ? record.remoteName
                                : record.originalRemoteName;
                        const QString originalSuffix =
                            nameForSuffix.contains(
                                QStringLiteral(".png.h264_"))
                                ? QStringLiteral("png")
                                : nameForSuffix.contains(
                                      QStringLiteral(".gif.h264_"))
                                    ? QStringLiteral("gif")
                                    : QStringLiteral("mp4");
                        QString replacementName;
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            const QString candidate =
                                h264PrinterNameForConversion(
                                generatedPrinterMediaName(originalSuffix),
                                record.printerProductId,
                                record.mediaConversion);
                            if (candidate != previousRemoteName &&
                                candidate != record.originalRemoteName &&
                                !files.contains(candidate)) {
                                replacementName = candidate;
                                break;
                            }
                        }
                        if (replacementName.isEmpty()) {
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Could not allocate a unique media name for the recovered transfer"),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                        record.remoteName = replacementName;
                        record.info.resultName = replacementName;
                        record.info.confirmedBytes = 0;
                        record.info.lastConfirmedChunkIndex = -1;
                        record.info.terminalOutcome.clear();
                        record.info.state =
                            QStringLiteral("Preflight");
                        record.info.stage =
                            QStringLiteral("EnsuringSession");
                        record.info.message = tr(
                            "Retry preflight completed; the preserved media will be transferred under a new device filename");
                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                        publishOperation(operationId);
                        dispatchPreparedUploadWithRetryBarrier(
                            currentPrinterPath(), operationId,
                            printerGeneration_);
                        return;
                    }
                    if (isSha256Hex(record.sourceContentSha256) &&
                        !record.conversionProfile.isEmpty()) {
                        const QString reusableName =
                            findReusableMediaOrigin(
                                record.sourceContentSha256,
                                record.conversionProfile, mediaFiles);
                        if (!reusableName.isEmpty() &&
                            reusableName != record.remoteName) {
                            const auto reusableMedia =
                                std::find_if(
                                    mediaFiles.cbegin(),
                                    mediaFiles.cend(),
                                    [&reusableName](
                                        const PrinterProtocol::MediaFile
                                            &media) {
                                        return media.name == reusableName;
                                    });
                            if (reusableMedia == mediaFiles.cend()) {
                                finishOperation(
                                    operationId,
                                    QStringLiteral("RetryAvailable"),
                                    QStringLiteral(
                                        "RetryCacheValidationFailed"),
                                    QStringLiteral("PreparedMedia"),
                                    tr("The reusable media origin was not present in the verified device file list"));
                                return;
                            }
                            record.remoteName = reusableName;
                            record.mediaFile = reusableName;
                            record.info.resultName = reusableName;
                            TryxRuntimeMediaEntry verifiedEntry;
                            verifiedEntry.name = reusableMedia->name;
                            verifiedEntry.size = reusableMedia->size;
                            verifiedEntry.source =
                                reusableMedia->source ==
                                    PrinterProtocol::MediaSource::Preset
                                ? 2U
                                : 1U;
                            verifiedEntry.readOnly =
                                reusableMedia->readOnly;
                            QString localErrorCategory;
                            QString localErrorMessage;
                            if (!commitVerifiedMediaMetadata(
                                    operationId, verifiedEntry,
                                    &localErrorCategory,
                                    &localErrorMessage)) {
                                updateMediaCatalog(mediaFiles);
                                emit mediaListUpdated(files);
                                auto failedCommit =
                                    operations_.find(operationId);
                                if (failedCommit != operations_.end()) {
                                    failedCommit->info.terminalOutcome =
                                        QStringLiteral("NotStarted");
                                    if (failedCommit->info
                                            .primaryErrorCategory
                                            .isEmpty()) {
                                        failedCommit->info
                                            .primaryErrorCategory =
                                            localErrorCategory;
                                        failedCommit->info
                                            .primaryErrorMessage =
                                            localErrorMessage;
                                    }
                                }
                                finishOperation(
                                    operationId,
                                    QStringLiteral("Failed"),
                                    localErrorCategory, QString(),
                                    localErrorMessage);
                                return;
                            }
                            if (!retryCacheSnapshot_.retryCandidate
                                     .has_value() ||
                                !consumeRetryCacheCandidate(
                                    retryCacheSnapshot_.retryCandidate
                                        ->operationId)) {
                                const bool durableCleanupPending =
                                    !retryCacheSnapshot_.cleanupPending
                                         .isEmpty() &&
                                    !retryCacheSnapshot_.retryCandidate
                                         .has_value() &&
                                    !retryCacheSnapshot_.inFlightDispatch
                                         .has_value();
                                if (durableCleanupPending) {
                                    updateMediaCatalog(mediaFiles);
                                    emit mediaListUpdated(files);
                                    const auto committed =
                                        operations_.constFind(operationId);
                                    const bool applyWasRequested =
                                        committed != operations_.constEnd() &&
                                        committed->info.applyAfterUpload;
                                    finishOperation(
                                        operationId,
                                        applyWasRequested
                                            ? QStringLiteral("Failed")
                                            : QStringLiteral("Succeeded"),
                                        QStringLiteral(
                                            "RetryCacheCleanupFailed"),
                                        QString(),
                                        applyWasRequested
                                            ? tr("A confirmed copy already exists, but Apply is blocked until local retry cleanup completes")
                                            : tr("A confirmed copy already exists; local retry cleanup remains pending and device mutations are blocked"));
                                    return;
                                }
                                finishOperation(
                                    operationId,
                                    QStringLiteral("RetryAvailable"),
                                    QStringLiteral(
                                        "RetryCacheCleanupFailed"),
                                    QStringLiteral("PreparedMedia"),
                                    tr("An existing copy was found, but the retry manifest could not be removed"));
                                return;
                            }
                            auto reused = operations_.find(operationId);
                            if (reused == operations_.end()) {
                                return;
                            }
                            removePreparedFileForOperation(operationId);
                            updateMediaCatalog(mediaFiles);
                            emit mediaListUpdated(files);
                            reused = operations_.find(operationId);
                            if (reused == operations_.end()) {
                                return;
                            }
                            if (reused->info.applyAfterUpload) {
                                reused->info.state =
                                    QStringLiteral("Applying");
                                reused->info.stage =
                                    QStringLiteral("ReusingExisting");
                                reused->info.message = tr(
                                    "A confirmed copy already exists; applying it without retransmission...");
                                publishOperation(operationId);
                                emit requestPrinterApplyMedia(
                                    currentPrinterPath(), reused->mediaFile,
                                    reused->applyRequest,
                                    reused->updateMetrics, QString(), {},
                                    operationId,
                                    printerGeneration_);
                                return;
                            }
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"), QString(),
                                QString(),
                                tr("A confirmed copy already exists; media data was not retransmitted"));
                            return;
                        }
                    }
                    if (exactPreparedFileCanBeTrusted) {
                        TryxRuntimeMediaEntry verifiedEntry;
                        verifiedEntry.name = exactMedia->name;
                        verifiedEntry.size = exactMedia->size;
                        verifiedEntry.source = 1U;
                        verifiedEntry.readOnly = exactMedia->readOnly;
                        QString localErrorCategory;
                        QString localErrorMessage;
                        if (!commitVerifiedMediaMetadata(
                                operationId, verifiedEntry,
                                &localErrorCategory,
                                &localErrorMessage)) {
                            updateMediaCatalog(mediaFiles);
                            emit mediaListUpdated(files);
                            auto failedCommit =
                                operations_.find(operationId);
                            if (failedCommit != operations_.end()) {
                                failedCommit->info.terminalOutcome =
                                    QStringLiteral("NotStarted");
                                if (failedCommit->info
                                        .primaryErrorCategory
                                        .isEmpty()) {
                                    failedCommit->info
                                        .primaryErrorCategory =
                                        localErrorCategory;
                                    failedCommit->info
                                        .primaryErrorMessage =
                                        localErrorMessage;
                                }
                            }
                            finishOperation(
                                operationId,
                                QStringLiteral("Failed"),
                                localErrorCategory, QString(),
                                localErrorMessage);
                            return;
                        }
                        const QString candidateOperationId =
                            retryCacheSnapshot_.retryCandidate
                                .has_value()
                            ? retryCacheSnapshot_.retryCandidate
                                  ->operationId
                            : QString();
                        if (!retryCacheSnapshot_.retryCandidate
                                 .has_value() ||
                            !consumeRetryCacheCandidate(
                                candidateOperationId)) {
                            const bool durableCleanupPending =
                                !retryCacheSnapshot_.cleanupPending
                                     .isEmpty() &&
                                !retryCacheSnapshot_.retryCandidate
                                     .has_value() &&
                                !retryCacheSnapshot_.inFlightDispatch
                                     .has_value();
                            if (durableCleanupPending) {
                                updateMediaCatalog(mediaFiles);
                                emit mediaListUpdated(files);
                                const auto committed =
                                    operations_.constFind(operationId);
                                const bool applyWasRequested =
                                    committed != operations_.constEnd() &&
                                    committed->info.applyAfterUpload;
                                if (committed != operations_.constEnd()) {
                                    emit mediaUploaded(
                                        committed->remoteName);
                                }
                                finishOperation(
                                    operationId,
                                    applyWasRequested
                                        ? QStringLiteral("Failed")
                                        : QStringLiteral("Succeeded"),
                                    QStringLiteral(
                                        "RetryCacheCleanupFailed"),
                                    QString(),
                                    applyWasRequested
                                        ? tr("The previous upload is present in FileList, but Apply is blocked until local retry cleanup completes")
                                        : tr("The previous upload is present in FileList; local retry cleanup remains pending and device mutations are blocked"));
                                return;
                            }
                            finishOperation(
                                operationId,
                                QStringLiteral("RetryAvailable"),
                                QStringLiteral(
                                    "RetryCacheCleanupFailed"),
                                QStringLiteral("PreparedMedia"),
                                tr("The media was verified, but the retry record could not be consumed"));
                            return;
                        }
                        auto verified = operations_.find(operationId);
                        if (verified == operations_.end()) {
                            return;
                        }
                        removePreparedFileForOperation(operationId);
                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                        verified = operations_.find(operationId);
                        if (verified == operations_.end()) {
                            return;
                        }
                        emit mediaUploaded(verified->remoteName);
                        if (verified->info.applyAfterUpload) {
                            verified->mediaFile = verified->remoteName;
                            verified->info.state = QStringLiteral("Applying");
                            verified->info.stage = QStringLiteral("Applying");
                            verified->info.message = tr(
                                "The previous upload is present in FileList; applying it without retransmission...");
                            publishOperation(operationId);
                            emit requestPrinterApplyMedia(
                                currentPrinterPath(), verified->mediaFile,
                                verified->applyRequest,
                                verified->updateMetrics,
                                QString(), {},
                                operationId, printerGeneration_);
                            return;
                        }
                        finishOperation(
                            operationId, QStringLiteral("Succeeded"),
                            QString(), QString(),
                            tr("The previous upload was verified in FileList; media data was not retransmitted"));
                        return;
                    }
                    if (files.contains(record.remoteName)) {
                        const QString originalSuffix =
                            record.remoteName.contains(QStringLiteral(".png.h264_"))
                                ? QStringLiteral("png")
                                : record.remoteName.contains(QStringLiteral(".gif.h264_"))
                                    ? QStringLiteral("gif")
                                    : QStringLiteral("mp4");
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            const QString candidate =
                                h264PrinterNameForConversion(
                                generatedPrinterMediaName(originalSuffix),
                                record.printerProductId,
                                record.mediaConversion);
                            if (!files.contains(candidate)) {
                                record.remoteName = candidate;
                                break;
                            }
                        }
                        if (files.contains(record.remoteName)) {
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Could not allocate a unique media name for retry"),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                        record.info.resultName = record.remoteName;
                    }
                    record.info.confirmedBytes = 0;
                    record.info.lastConfirmedChunkIndex = -1;
                    record.info.terminalOutcome.clear();
                    record.info.state = QStringLiteral("Preflight");
                    record.info.stage = QStringLiteral("EnsuringSession");
                    record.info.message = tr("Retry preflight completed");
                    updateMediaCatalog(mediaFiles);
                    emit mediaListUpdated(files);
                    publishOperation(operationId);
                    dispatchPreparedUploadWithRetryBarrier(
                        currentPrinterPath(), operationId,
                        printerGeneration_);
                    return;
                }
                if (record.info.stage != QStringLiteral("RefreshingMedia")) {
                    return;
                }
                if (!exactPreparedFilePresent) {
                    handlePreparedUploadFailure(
                        operationId,
                        files.contains(record.remoteName)
                            ? tr("The device acknowledged upload completion, but %1 does not match the prepared file size or source in FileList")
                                  .arg(record.remoteName)
                            : tr("The device acknowledged upload completion, but %1 is absent from FileList")
                                  .arg(record.remoteName),
                        PrinterProtocol::MutationOutcome::PartialOrUnknown);
                    return;
                }

                TryxRuntimeMediaEntry verifiedEntry;
                verifiedEntry.name = exactMedia->name;
                verifiedEntry.size = exactMedia->size;
                verifiedEntry.source = 1U;
                verifiedEntry.readOnly = exactMedia->readOnly;
                if (!persistVerifiedReplaceUploadBeforeRetirement()) {
                    return;
                }
                const bool localMetadataRequired =
                    !record.stagedThumbnailPath.isEmpty() ||
                    record.ensureExisting ||
                    isSha256Hex(record.sourceContentSha256) ||
                    record.replaceOperation;
                if (localMetadataRequired) {
                    QString storeError;
                    if (!beginRetryCacheLocalCommit(
                            operationId, verifiedEntry,
                            &storeError)) {
                        finishOperation(
                            operationId,
                            QStringLiteral("Failed"),
                            QStringLiteral(
                                "RetryCacheLocalCommitFailed"),
                            QString(), storeError);
                        return;
                    }
                    QString localErrorCategory;
                    QString localErrorMessage;
                    if (!commitVerifiedMediaMetadata(
                            operationId, verifiedEntry,
                            &localErrorCategory,
                            &localErrorMessage)) {
                        QString deferError;
                        if (!deferRetryCacheLocalCommit(
                                operationId,
                                localErrorCategory,
                                localErrorMessage,
                                &deferError)) {
                            finishOperation(
                                operationId,
                                QStringLiteral("Failed"),
                                QStringLiteral(
                                    "RetryCacheLocalCommitFailed"),
                                QString(),
                                deferError.isEmpty()
                                    ? localErrorMessage
                                    : deferError);
                            return;
                        }
                        auto deferred = operations_.find(operationId);
                        if (deferred != operations_.end()) {
                            deferred->info.terminalOutcome =
                                QStringLiteral("NotStarted");
                            deferred->requiresDeviceRecovery = false;
                            deferred->retryMustUseNewRemoteName = false;
                            deferred
                                ->uploadFinalizationReconciliationPending =
                                false;
                            if (deferred->info
                                    .primaryErrorCategory
                                    .isEmpty()) {
                                deferred->info.primaryErrorCategory =
                                    localErrorCategory;
                                deferred->info.primaryErrorMessage =
                                    localErrorMessage;
                            }
                        }
                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                        finishOperation(
                            operationId,
                            QStringLiteral("RetryAvailable"),
                            localErrorCategory,
                            QStringLiteral("PreparedMedia"),
                            localErrorMessage);
                        return;
                    }
                }

                QString retirementError;
                bool retirementCleanupPending = false;
                if (!retireRetryCacheDispatch(
                        operationId,
                        tryx::RetryCacheStore::DispatchRetirement::
                            AcknowledgedSuccess,
                        &retirementError)) {
                    retirementCleanupPending =
                        retryCacheDispatchRetiredIntoCleanup(
                            retryCacheSnapshot_);
                    if (!retirementCleanupPending) {
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("RetryCacheRetirementFailed"),
                            QString(), retirementError.isEmpty()
                                ? tr("FileList verified the upload, but its durable retry record could not be retired")
                                : retirementError);
                        return;
                    }
                }
                finishVerifiedUploadAfterRetirement(
                    retirementCleanupPending, false);
            });
    connect(worker_, &DeviceWorker::printerMediaListFailed, this,
            [this, printerResultIsCurrent,
             printerOperationResultIsExpected](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (operationId.isEmpty()) {
                    if (printerResultIsCurrent(generation)) {
                        emit deviceError(message);
                    }
                    return;
                }
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (record.originLookupPending) {
                    record.originLookupPending = false;
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("FileListUnavailable"), QString(),
                        tr("The existing-media check failed; upload was not started: %1")
                            .arg(message));
                    return;
                }
                handlePreparedUploadFailure(
                    operationId, message,
                    record.retryPreflight
                        ? PrinterProtocol::MutationOutcome::NotStarted
                        : PrinterProtocol::MutationOutcome::PartialOrUnknown);
            });
    connect(worker_, &DeviceWorker::printerDeleteFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId,
                const QStringList &requestedNames,
                const QStringList &deletedNames,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (record.replaceOperation &&
                    (generation != printerGeneration_ ||
                     record.deviceChangePending ||
                     record.uploadDeviceIdentity.trimmed().isEmpty() ||
                     record.uploadDeviceIdentity.trimmed() !=
                         printerDeviceSerial_.trimmed())) {
                    record.info.terminalOutcome =
                        QStringLiteral("PartialOrUnknown");
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("ReconcileOnly"),
                        record.deviceChangeMessage.isEmpty()
                            ? tr("The PASE connection changed before the replacement delete result could be associated with the original device")
                            : record.deviceChangeMessage);
                    return;
                }
                record.deletedNames = deletedNames;
                const bool replaceJournalOnlyReconciliation =
                    record.replaceOperation &&
                    record.deleteReconcileOnly &&
                    pendingDeleteOperationId_.isEmpty();
                if (!record.deviceChangePending &&
                    (success ||
                     outcome == PrinterProtocol::MutationOutcome::Rejected ||
                     outcome ==
                         PrinterProtocol::MutationOutcome::NotStarted)) {
                    QStringList names;
                    QSet<QString> seen;
                    for (const PrinterProtocol::MediaFile &media :
                         mediaFiles) {
                        if (!seen.contains(media.name)) {
                            seen.insert(media.name);
                            names.append(media.name);
                        }
                    }
                    updateMediaCatalog(mediaFiles);
                    emit mediaListUpdated(names);
                }
                const auto freshCatalogHasExactWritableUserMedia =
                    [&mediaFiles](const QString &name,
                                  quint64 size) {
                        int nameMatches = 0;
                        bool exactMatch = false;
                        for (const auto &media :
                             mediaFiles) {
                            if (media.name != name) {
                                continue;
                            }
                            ++nameMatches;
                            exactMatch =
                                static_cast<quint64>(
                                    media.size) ==
                                    size &&
                                media.source ==
                                    PrinterProtocol::
                                        MediaSource::User &&
                                !media.readOnly;
                        }
                        return nameMatches == 1 &&
                               exactMatch;
                    };
                const bool replacementCopyMatchesFreshCatalog =
                    !record.replaceOperation ||
                    (record.replaceJournalActive &&
                     PrinterProtocol::isSafeUploadMediaName(
                         record.replaceJournal.newRemoteName) &&
                     record.replaceJournal.newSize > 0 &&
                     freshCatalogHasExactWritableUserMedia(
                         record.replaceJournal.newRemoteName,
                         record.replaceJournal.newSize));
                if (!replacementCopyMatchesFreshCatalog) {
                    record.info.terminalOutcome =
                        QStringLiteral("PartialOrUnknown");
                    record.info.resultName = record.remoteName;
                    record.replaceJournal.disposition =
                        QStringLiteral("PartialOrUnknown");
                    QString journalError;
                    if (!writeReplaceJournal(
                            operationId,
                            QStringLiteral(
                                "DeleteReconciliation"),
                            &journalError)) {
                        qWarning().noquote()
                            << "Cannot persist unresolved replacement-copy identity:"
                            << journalError;
                    }
                    const bool hasDeleteIntent =
                        pendingDeleteOperationId_ ==
                        operationId;
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        hasDeleteIntent
                            ? QStringLiteral(
                                  "DeleteReconcile")
                            : QStringLiteral(
                                  "ReconcileOnly"),
                        tr("The fresh FileList does not contain the exact verified replacement copy. Replace remains unresolved and no mutation was repeated."));
                    return;
                }
                if (success) {
                    QString clearError;
                    if (!clearDeleteIntent(operationId, &clearError)) {
                        pendingDeleteOperationId_ = operationId;
                        finishOperation(
                            operationId,
                            QStringLiteral("RetryAvailable"),
                            QStringLiteral("PersistenceFailed"),
                            QStringLiteral("DeleteReconcile"),
                            tr("Deletion is confirmed, but its intent journal could not be removed: %1")
                                .arg(clearError));
                        return;
                    }
                    if (record.replaceOperation) {
                        record.info.terminalOutcome =
                            QStringLiteral("Replaced");
                        finishOperation(
                            operationId,
                            QStringLiteral("Succeeded"),
                            QString(), QString(),
                            tr("Replacement uploaded, applied and the original media was deleted"));
                        return;
                    }
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        requestedNames.size() == 1
                            ? tr("Media file deleted and verified through FileList")
                            : tr("%1 media files deleted and verified through FileList")
                                  .arg(requestedNames.size()));
                    return;
                }
                if (outcome ==
                    PrinterProtocol::MutationOutcome::PartialOrUnknown) {
                    if (replaceJournalOnlyReconciliation) {
                        const QString target =
                            requestedNames.value(0);
                        const bool targetStillPresent =
                            freshCatalogHasExactWritableUserMedia(
                                target,
                                record.replaceJournal.originalSize);
                        if (targetStillPresent) {
                            QString clearError;
                            if (!clearDeleteIntent(operationId,
                                                   &clearError)) {
                                finishOperation(
                                    operationId,
                                    QStringLiteral("RetryAvailable"),
                                    QStringLiteral("PersistenceFailed"),
                                    QStringLiteral("ReconcileOnly"),
                                    tr("The original media is still present, but stale delete state could not be cleared: %1")
                                        .arg(clearError));
                                return;
                            }
                            record.info.terminalOutcome =
                                QStringLiteral("NewCopyReady");
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"),
                                QStringLiteral("OriginalRetained"),
                                QString(),
                                tr("Read-only FileList confirmed that the original media is still present. FileRemove was not repeated."));
                            return;
                        }
                        record.info.terminalOutcome =
                            QStringLiteral("PartialOrUnknown");
                        record.replaceJournal.disposition =
                            QStringLiteral("PartialOrUnknown");
                        QString journalError;
                        if (!writeReplaceJournal(
                                operationId,
                                QStringLiteral("DeleteReconciliation"),
                                &journalError)) {
                            qWarning().noquote()
                                << "Cannot persist read-only Replace delete reconciliation:"
                                << journalError;
                        }
                        finishOperation(
                            operationId,
                            QStringLiteral("RetryAvailable"),
                            QStringLiteral("PartialOrUnknown"),
                            QStringLiteral("ReconcileOnly"),
                            errorMessage.isEmpty()
                                ? tr("The previous FileRemove outcome is still unknown; only FileList reconciliation may be retried")
                                : tr("The previous FileRemove outcome is still unknown: %1")
                                      .arg(errorMessage));
                        return;
                    }
                    const int currentIndex = qBound(
                        0, deletedNames.size(),
                        qMax(0, requestedNames.size() - 1));
                    const QString currentName = requestedNames.value(
                        currentIndex, record.info.resultName);
                    pendingDeleteOperationId_ = operationId;
                    const auto loadedIntent =
                        tryx::DeleteIntentStore(deleteIntentPath()).load();
                    if (loadedIntent.loaded() &&
                        loadedIntent.record.operationId == operationId) {
                        pendingDeleteIntent_ = loadedIntent.record;
                    } else if (pendingDeleteIntent_.has_value() &&
                               pendingDeleteIntent_->operationId ==
                                   operationId) {
                        pendingDeleteIntent_->currentIndex = currentIndex;
                        pendingDeleteIntent_->currentName = currentName;
                        pendingDeleteIntent_->deletedNames = deletedNames;
                        pendingDeleteIntent_->mayHaveStarted = true;
                    }
                    record.info.resultName = currentName;
                    if (record.replaceOperation) {
                        record.info.terminalOutcome =
                            QStringLiteral(
                                "PartialOrUnknown");
                        record.info.resultName =
                            record.remoteName;
                        record.replaceJournal.disposition =
                            QStringLiteral(
                                "PartialOrUnknown");
                        QString journalError;
                        if (!writeReplaceJournal(
                                operationId,
                                QStringLiteral(
                                    "DeleteReconciliation"),
                                &journalError)) {
                            qWarning().noquote()
                                << "Cannot persist uncertain Replace delete outcome:"
                                << journalError;
                        }
                    }
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("DeleteReconcile"),
                        tr("Delete outcome is unknown. FileRemove will not be repeated; only FileList reconciliation is allowed: %1")
                            .arg(errorMessage));
                    return;
                }

                QString clearError;
                if (!clearDeleteIntent(operationId, &clearError)) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PersistenceFailed"),
                        QStringLiteral("DeleteReconcile"),
                        tr("Delete did not complete, but its intent journal could not be cleared: %1")
                            .arg(clearError));
                    return;
                }
                if (record.replaceOperation) {
                    record.info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId,
                        QStringLiteral("Succeeded"),
                        QStringLiteral("OriginalRetained"),
                        QString(),
                        tr("The replacement is active, but the original media was retained: %1")
                            .arg(errorMessage));
                    return;
                }
                const QString terminalState =
                    outcome == PrinterProtocol::MutationOutcome::Cancelled
                        ? QStringLiteral("Cancelled")
                        : QStringLiteral("Failed");
                finishOperation(operationId, terminalState,
                                mutationOutcomeName(outcome), QString(),
                                errorMessage);
            });
    connect(worker_, &DeviceWorker::printerSavedLayoutProofFailed, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId,
                const QString &errorCategory,
                const QString &errorMessage,
                quint64 generation) {
                if (!printerOperationResultIsExpected(
                        operationId, generation)) {
                    return;
                }
                auto found = operations_.find(operationId);
                if (found == operations_.end()) {
                    return;
                }
                found->info.state = QStringLiteral("Failed");
                found->info.stage = QStringLiteral("Rejected");
                found->info.errorCategory = errorCategory;
                found->info.terminalOutcome =
                    QStringLiteral("NotStarted");
                found->info.retryMode.clear();
                found->info.message = errorMessage;
                emit requestEndPrinterForegroundOperation(
                    operationId, found->info.deviceGeneration);
                if (activeOperationId_ == operationId) {
                    activeOperationId_.clear();
                }
                worker_->clearPrinterOperationCancellation(operationId);
                publishOperation(operationId);
                pruneOperationHistory();
            });
    connect(worker_, &DeviceWorker::printerApplyFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, const QString &mediaFile,
                bool success, bool metricsUpdated,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.resultName = mediaFile;
                if (success) {
                    if (record.deviceChangePending) {
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("DeviceChanged"),
                            QStringLiteral("ReconcileOnly"),
                            record.deviceChangeMessage.isEmpty()
                                ? tr("USB changed before the applied PASE configuration could be associated with its original device")
                                : record.deviceChangeMessage);
                        return;
                    }
                    if (metricsUpdated) {
                        PrinterProtocol::PaseOverlayConfig overlay =
                            record.updateMetrics ||
                                    record.applyRequest.replaceOverlay
                                ? paseOverlayFromApplyRequest(
                                      record.applyRequest)
                                : persistedPaseOverlayForDevice(
                                      printerDeviceSerial_);
                        if (record.applyRequest.display.orientationPresent) {
                            overlay.waterfallMode =
                                record.applyRequest.display.waterfallMode;
                        }
                        QString metricsPersistenceError;
                        if (!persistPaseMetricsConfiguration(
                                overlay, paseOverlayHasContent(overlay),
                                &metricsPersistenceError)) {
                            metricsState_.deviceSerial =
                                printerDeviceSerial_.trimmed();
                            metricsState_.enabled =
                                paseOverlayHasMetrics(overlay);
                            metricsState_.samplingActive =
                                paseOverlayHasMetrics(overlay);
                            metricsState_.metrics =
                                overlay.left.metrics;
                            metricsState_.alignment =
                                overlay.left.alignment;
                            metricsState_.textColor =
                                overlay.left.textColor;
                            metricsState_.diagnostic = tr(
                                "The PASE metrics layout was applied but could not be persisted: %1")
                                                           .arg(metricsPersistenceError);
                            publishMetricsState();
                            finishOperation(
                                operationId, QStringLiteral("Failed"),
                                QStringLiteral("PersistenceFailed"),
                                QStringLiteral("ReconcileOnly"),
                                metricsState_.diagnostic);
                            emit screenConfigChanged();
                            return;
                        }
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled =
                            paseOverlayHasMetrics(overlay);
                        metricsState_.samplingActive =
                            paseOverlayHasMetrics(overlay);
                        metricsState_.metrics =
                            overlay.left.metrics;
                        metricsState_.alignment =
                            overlay.left.alignment;
                        metricsState_.textColor =
                            overlay.left.textColor;
                        metricsState_.diagnostic.clear();
                        publishMetricsState();
                        if (displayState_.valid) {
                            PrinterProtocol::PaseDisplayState state;
                            state.backlightEnabled =
                                displayState_.backlightEnabled;
                            state.brightness =
                                displayState_.brightness;
                            state.standbyEnabled =
                                displayState_.standbyEnabled;
                            state.standbyMedia =
                                displayState_.standbyMedia;
                            state.mirrorMode =
                                displayState_.mirrorMode;
                            state.waterfallMode =
                                displayState_.waterfallMode;
                            state.screenMode =
                                displayState_.screenMode;
                            state.playMode =
                                displayState_.playMode;
                            state.media = displayState_.media;
                            updateDisplayState(state, overlay);
                        }
                    }
                    if (record.replaceOperation) {
                        record.replaceJournal.applyVerified = true;
                        QString journalError;
                        if (!writeReplaceJournal(
                                operationId,
                                QStringLiteral("ApplyVerification"),
                                &journalError)) {
                            record.info.terminalOutcome =
                                QStringLiteral("NewCopyReady");
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"),
                                QStringLiteral(
                                    "OriginalRetained"),
                                QString(),
                                tr("The new copy is active, but the original was retained because Apply verification could not be persisted: %1")
                                    .arg(journalError));
                            emit screenConfigChanged();
                            return;
                        }
                        if (!writeReplaceJournal(
                                operationId,
                                QStringLiteral(
                                    "ReferenceReconciliation"),
                                &journalError)) {
                            record.info.terminalOutcome =
                                QStringLiteral("NewCopyReady");
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"),
                                QStringLiteral(
                                    "OriginalRetained"),
                                QString(),
                                tr("The new copy is active, but the original was retained because reference reconciliation could not be persisted: %1")
                                    .arg(journalError));
                            emit screenConfigChanged();
                            return;
                        }
                        record.info.state =
                            QStringLiteral("Preflight");
                        record.info.stage =
                            QStringLiteral(
                                "ReconcilingReferences");
                        record.info.message = tr(
                            "The replacement is active; re-reading every device reference before deletion...");
                        publishOperation(operationId);
                        emit requestPrinterReplacePreflight(
                            currentPrinterPath(),
                            record.originalRemoteNameForReplace,
                            static_cast<qint64>(
                                record.replaceJournal.originalSize),
                            record.replaceJournal.newRemoteName,
                            static_cast<qint64>(
                                record.replaceJournal.newSize),
                            operationId,
                            printerGeneration_);
                        emit screenConfigChanged();
                        return;
                    }
                    finishOperation(operationId, QStringLiteral("Succeeded"),
                                    QString(), QString(),
                                    record.cancelRequested
                                        ? tr("Media was applied before cancellation completed")
                                        : tr("Media applied successfully"));
                    emit screenConfigChanged();
                    return;
                }
                if (record.replaceOperation) {
                    const bool applyOutcomeUnknown =
                        outcome ==
                            PrinterProtocol::MutationOutcome::
                                PartialOrUnknown ||
                        outcome ==
                            PrinterProtocol::MutationOutcome::
                                FinalizationUnknown ||
                        outcome ==
                            PrinterProtocol::MutationOutcome::
                                VerificationFailed;
                    if (applyOutcomeUnknown) {
                        record.replaceJournal.disposition =
                            QStringLiteral("PartialOrUnknown");
                        QString journalError;
                        if (!writeReplaceJournal(
                                operationId,
                                QStringLiteral("ApplyVerification"),
                                &journalError)) {
                            qWarning().noquote()
                                << "Cannot persist uncertain Replace Apply outcome:"
                                << journalError;
                        }
                    }
                    record.info.terminalOutcome =
                        applyOutcomeUnknown
                            ? QStringLiteral("PartialOrUnknown")
                            : QStringLiteral("NewCopyReady");
                    finishOperation(
                        operationId,
                        applyOutcomeUnknown
                            ? QStringLiteral("RetryAvailable")
                            : QStringLiteral("Succeeded"),
                        applyOutcomeUnknown
                            ? QStringLiteral("PartialOrUnknown")
                            : QStringLiteral("OriginalRetained"),
                        applyOutcomeUnknown
                            ? QStringLiteral("ReconcileOnly")
                            : QString(),
                        applyOutcomeUnknown
                            ? tr("The new copy is present, but the Apply outcome is uncertain. The original was not deleted: %1")
                                  .arg(errorMessage)
                            : tr("The new copy is ready, but Apply did not complete. The original was retained: %1")
                                  .arg(errorMessage));
                    return;
                }
                if (record.cancelRequested &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(operationId, QStringLiteral("Cancelled"),
                                    QStringLiteral("UserCancelled"),
                                    QString(),
                                    tr("Operation cancelled by the user"));
                    return;
                }
                if (record.deviceChangePending &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(operationId, QStringLiteral("Cancelled"),
                                    QStringLiteral("DeviceChanged"),
                                    QString(), record.deviceChangeMessage);
                    return;
                }
                const QString retryMode =
                    outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
                        ? QStringLiteral("ReconcileOnly")
                        : QString();
                const QString category = mutationOutcomeName(outcome);
                finishOperation(operationId, QStringLiteral("Failed"),
                                category, retryMode, errorMessage);
            });
    connect(worker_, &DeviceWorker::printerMetricsConfigured, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (success && record.deviceChangePending) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("DeviceChanged"),
                        QStringLiteral("ReconcileOnly"),
                        record.deviceChangeMessage.isEmpty()
                            ? tr("USB changed before the PASE metrics layout could be associated with its original device")
                            : record.deviceChangeMessage);
                    return;
                }
                if (success) {
                    const PrinterProtocol::PaseOverlayConfig overlay =
                        paseOverlayFromMetricsRequest(record.metricsRequest);
                    QString persistenceError;
                    if (!persistPaseMetricsConfiguration(
                            overlay, record.metricsRequest.enabled,
                            &persistenceError)) {
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled = record.metricsRequest.enabled;
                        metricsState_.samplingActive =
                            record.metricsRequest.enabled;
                        metricsState_.metrics =
                            overlay.left.metrics;
                        metricsState_.alignment =
                            overlay.left.alignment;
                        metricsState_.textColor =
                            overlay.left.textColor;
                        metricsState_.diagnostic = tr(
                            "The PASE metrics layout was applied but could not be persisted: %1")
                                                       .arg(persistenceError);
                        publishMetricsState();
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("PersistenceFailed"),
                            QStringLiteral("ReconcileOnly"),
                            metricsState_.diagnostic);
                        return;
                    }
                    metricsState_.deviceSerial =
                        printerDeviceSerial_.trimmed();
                    metricsState_.enabled = record.metricsRequest.enabled;
                    metricsState_.samplingActive =
                        record.metricsRequest.enabled;
                    metricsState_.metrics =
                        overlay.left.metrics;
                    metricsState_.alignment =
                        overlay.left.alignment;
                    metricsState_.textColor =
                        overlay.left.textColor;
                    metricsState_.diagnostic.clear();
                    publishMetricsState();
                    if (displayState_.valid) {
                        PrinterProtocol::PaseDisplayState state;
                        state.backlightEnabled =
                            displayState_.backlightEnabled;
                        state.brightness =
                            displayState_.brightness;
                        state.standbyEnabled =
                            displayState_.standbyEnabled;
                        state.standbyMedia =
                            displayState_.standbyMedia;
                        state.mirrorMode =
                            displayState_.mirrorMode;
                        state.waterfallMode =
                            displayState_.waterfallMode;
                        state.screenMode =
                            displayState_.screenMode;
                        state.playMode =
                            displayState_.playMode;
                        state.media = displayState_.media;
                        updateDisplayState(state, overlay);
                    }
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        record.metricsRequest.enabled
                            ? tr("PASE metrics configured successfully")
                            : tr("PASE metrics disabled successfully"));
                    return;
                }
                if (record.cancelRequested &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(
                        operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("UserCancelled"), QString(),
                        tr("Operation cancelled by the user"));
                    return;
                }
                const QString retryMode =
                    outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
                        ? QStringLiteral("ReconcileOnly")
                        : QString();
                finishOperation(operationId, QStringLiteral("Failed"),
                                mutationOutcomeName(outcome), retryMode,
                                errorMessage);
            });
    connect(worker_, &DeviceWorker::printerMetricsAvailabilityChanged, this,
            [this, printerResultIsCurrent](
                const QStringList &availableMetrics, quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    metricsState_.availableMetrics == availableMetrics) {
                    return;
                }
                metricsState_.availableMetrics = availableMetrics;
                publishMetricsState();
            });
    connect(worker_, &DeviceWorker::printerScreenConfigSet, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit screenConfigChanged();
                }
            });
    connect(worker_, &DeviceWorker::printerDeviceVersionsReady, this,
            [this, printerResultIsCurrent](const QString &firmware,
                                           const QString &appVersion,
                                           quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit printerDeviceVersionsReady(firmware, appVersion);
                }
            });
    connect(
        worker_, &DeviceWorker::printerDeviceSpecificationsReady,
        this,
        [this, printerResultIsCurrent](
            const PrinterProtocol::DeviceSpecifications &specifications,
            const QString &devicePath, const QString &deviceSerial,
            quint16 productId, quint64 generation) {
            const QString identity = deviceSerial.trimmed();
            const bool exactDiscoveryContext =
                printerSnapshot_.devices.size() == 1 &&
                printerSnapshot_.devices.first().devicePath == devicePath &&
                printerSnapshot_.devices.first().serial.trimmed() == identity &&
                printerSnapshot_.devices.first().productId == productId;
            if (!printerResultIsCurrent(generation) ||
                generation == 0 ||
                devicePath != printerDevicePath_ ||
                identity.isEmpty() ||
                identity != printerDeviceSerial_.trimmed() ||
                productId != printerProductId_ ||
                (productId != 0x1011 && productId != 0x1021) ||
                !exactDiscoveryContext) {
                return;
            }

            clearDeviceSpecificationsCache();
            const bool complete =
                specifications.valid &&
                !specifications.reportedProductName.isEmpty() &&
                specifications.reportedProductName.size() <= 128 &&
                specifications.videoOutputWidth >= 1 &&
                specifications.videoOutputWidth <= 16384 &&
                specifications.videoOutputHeight >= 1 &&
                specifications.videoOutputHeight <= 16384 &&
                (specifications.screenType == QStringLiteral("LCD") ||
                 specifications.screenType == QStringLiteral("OLED"));
            if (!complete) {
                return;
            }

            deviceSpecificationsCache_ = specifications;
            deviceSpecificationsDevicePath_ = devicePath;
            deviceSpecificationsDeviceIdentity_ = identity;
            deviceSpecificationsProductId_ = productId;
            deviceSpecificationsGeneration_ = generation;
        });
    connect(worker_, &DeviceWorker::printerDeviceInfoReady, this,
            [this, printerResultIsCurrent](const PrinterProtocol::DeviceInfo &info,
                                           quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit printerDeviceVersionsReady(
                        info.firmwareVersion, info.appVersion);
                    emit printerDeviceInfoReady(info);
                }
            });
    connect(worker_, &DeviceWorker::printerDeviceInfoFailed, this,
            [this, printerResultIsCurrent](const QString &message, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit printerDeviceInfoFailed(message);
                }
            });
    connect(worker_, &DeviceWorker::printerDisplayStateReady, this,
            [this, printerResultIsCurrent](
                const PrinterProtocol::PaseDisplayState &state,
                quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                updateDisplayState(
                    state,
                    persistedPaseOverlayForDevice(
                        printerDeviceSerial_));
            });
    connect(worker_, &DeviceWorker::printerDisplayStateFailed, this,
            [this, printerResultIsCurrent](
                const QString &message, quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                displayState_.deviceSerial =
                    printerDeviceSerial_.trimmed();
                displayState_.valid = false;
                displayState_.diagnostic =
                    tr("Failed to read PASE display state: %1")
                        .arg(message);
                publishDisplayState();
            });
#ifdef TRYX_PROTOCOL_TESTING
    connect(worker_, &DeviceWorker::printerDeviceInfoFailed, this,
            &DeviceManager::printerWorkerDeviceInfoFailedForTesting);
#endif

    connect(this, &DeviceManager::requestConnect,
            worker_, &DeviceWorker::connectDevice);
    connect(this, &DeviceManager::requestDisconnect,
            worker_, &DeviceWorker::disconnectDevice);
    connect(this, &DeviceManager::requestBrightness,
            worker_, &DeviceWorker::setBrightness);
    connect(this, &DeviceManager::requestScreenConfig,
            worker_, &DeviceWorker::setScreenConfig);
    connect(this, &DeviceManager::requestDeleteMedia,
            worker_, &DeviceWorker::deleteMedia);
    connect(this, &DeviceManager::requestUploadMedia,
            worker_, &DeviceWorker::uploadMedia);
    connect(this, &DeviceManager::requestRefreshMedia,
            worker_, &DeviceWorker::refreshMediaList);
    connect(this, &DeviceManager::requestKeepalive,
            worker_, &DeviceWorker::sendKeepalive);
    connect(this, &DeviceManager::requestRotation,
            worker_, &DeviceWorker::setRotation);
    connect(this, &DeviceManager::requestReboot,
            worker_, &DeviceWorker::rebootDevice);
    connect(this, &DeviceManager::requestSysinfo,
            worker_, &DeviceWorker::sendSysinfo);
    connect(this, &DeviceManager::requestConfigurePrinter,
            worker_, &DeviceWorker::configurePrinterDevice);
    connect(this, &DeviceManager::requestRestorePrinterOverlay,
            worker_, &DeviceWorker::restorePrinterOverlay);
    connect(this, &DeviceManager::requestBeginPrinterForegroundOperation,
            worker_, &DeviceWorker::beginPrinterForegroundOperation);
    connect(this, &DeviceManager::requestEndPrinterForegroundOperation,
            worker_, &DeviceWorker::endPrinterForegroundOperation);
    connect(this, &DeviceManager::requestClearPrinter,
            worker_, &DeviceWorker::clearPrinterDevice);
    connect(this, &DeviceManager::requestFirmwareTransportQuiesce,
            worker_, &DeviceWorker::quiesceForFirmware);
    connect(
        this,
        &DeviceManager::requestFirmwareQuiesceReleaseFence,
        worker_,
        &DeviceWorker::releaseFirmwareQuiesceFence);
    connect(this, &DeviceManager::requestPrinterDeviceInfo,
            worker_, &DeviceWorker::readPrinterDeviceInfo);
    connect(this, &DeviceManager::requestPrinterDisplayState,
            worker_, &DeviceWorker::readPrinterDisplayState);
    connect(this, &DeviceManager::requestAnalyzePrinterSource,
            printerMediaPreparer_, &PrinterMediaPreparer::analyzeSource);
    connect(
        this,
        &DeviceManager::requestAnalyzePrinterSourceWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::analyzeSourceWithPreparationProfile);
    connect(this, &DeviceManager::requestPrinterUploadPrepared,
            worker_, &DeviceWorker::uploadPreparedPrinterMedia);
    connect(this, &DeviceManager::requestPrinterRefreshMedia,
            worker_, &DeviceWorker::refreshPrinterMediaList);
    connect(this, &DeviceManager::requestPrinterStageMedia,
            worker_, &DeviceWorker::stagePrinterMedia);
    connect(this, &DeviceManager::requestPrinterReplacePreflight,
            worker_, &DeviceWorker::preflightReplacePrinterMedia);
    connect(this, &DeviceManager::requestPrinterDeleteMedia,
            worker_, &DeviceWorker::deletePrinterMedia);
    connect(this, &DeviceManager::requestPrinterApplyMedia,
            worker_, &DeviceWorker::applyPrinterMedia);
    connect(this, &DeviceManager::requestPrinterConfigureMetrics,
            worker_, &DeviceWorker::configurePrinterMetrics);
    connect(this, &DeviceManager::requestPrinterSysinfo,
            worker_, &DeviceWorker::sendPrinterSysinfo);
    if (automaticPrinterSessionStart_) {
        connect(this, &DeviceManager::requestStartPrinterSession,
                worker_, &DeviceWorker::startPrinterDisplaySession);
    }
    connect(worker_, &DeviceWorker::sysinfoSent, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit sysinfoSent();
                }
            });
    connect(worker_, &DeviceWorker::printerSysinfoSent, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit sysinfoSent();
                }
            });
    connect(worker_, &DeviceWorker::printerSysinfoFailed, this,
            [this, printerResultIsCurrent](const QString &message,
                                           quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit deviceError(
                        tr("Failed to update PASE metrics: %1").arg(message));
                }
            });
    connect(worker_, &DeviceWorker::printerTransportReady, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation) &&
                    printerDisplaySessionActive_ &&
                    isPrinterClassDevicePresent() &&
                    activeOperationId_.isEmpty()) {
                    emit printerTransportReady();
                }
            });

    connect(this, &DeviceManager::requestPreparePrinterMedia,
            printerMediaPreparer_, &PrinterMediaPreparer::prepare);
    connect(
        this,
        &DeviceManager::requestPreparePrinterMediaWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::prepareWithPreparationProfile);
    connect(this, &DeviceManager::requestPrepareRecoveredPrinterMedia,
            printerMediaPreparer_,
            &PrinterMediaPreparer::prepareRecovered);
    connect(
        this,
        &DeviceManager::requestPrepareRecoveredPrinterMediaWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::prepareRecoveredWithPreparationProfile);
    connect(
        this, &DeviceManager::requestCancelPrinterPreparation,
        this,
        [this](quint64 generation) {
            printerMediaPreparer_->requestGenerationCancellation(generation);
        },
        Qt::DirectConnection);
    connect(this, &DeviceManager::requestCancelPrinterPreparation,
            printerMediaPreparer_, &PrinterMediaPreparer::cancelStale);
    connect(
        this, &DeviceManager::requestCancelPrinterPreparationOperation,
        this,
        [this](const QString &operationId) {
            printerMediaPreparer_->requestOperationCancellation(operationId);
        },
        Qt::DirectConnection);
    connect(this, &DeviceManager::requestCancelPrinterPreparationOperation,
            printerMediaPreparer_, &PrinterMediaPreparer::cancelOperation);
    connect(this, &DeviceManager::requestReleasePrinterPreparation,
            printerMediaPreparer_, &PrinterMediaPreparer::releasePreparedFile);
    connect(
        this,
        &DeviceManager::requestValidatePrinterRetryCacheArtifact,
        printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCacheArtifact);
    connect(
        printerMediaPreparer_,
        &PrinterMediaPreparer::retryCacheArtifactValidated,
        this,
        &DeviceManager::handleRetryCacheArtifactValidation);
    connect(worker_, &DeviceWorker::printerPreparedFileConsumed, this,
            [this](const QString &uploadPath) {
                releasePrinterPreparationPath(uploadPath);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::sourceAnalyzed,
            this,
            [this, printerResultIsCurrent](
                const QString &operationId, const QString &localPath,
                const QString &contentSha256, qint64 sourceSize,
                const QString &conversionProfile, quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (!record.ensureExisting || record.cancelRequested ||
                    !isSha256Hex(contentSha256) || sourceSize <= 0 ||
                    conversionProfile.isEmpty() ||
                    QFileInfo(localPath).absoluteFilePath() !=
                        record.sourcePath) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceAnalysisFailed"), QString(),
                        tr("Source media identity could not be associated with the active operation"));
                    return;
                }
                const QString completedFingerprint =
                    sourceFingerprint(localPath);
                if (completedFingerprint.isEmpty() ||
                    completedFingerprint != record.sourceFingerprint) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceChanged"), QString(),
                        tr("Source media changed while its identity was being calculated"));
                    return;
                }
                record.sourceContentSha256 = contentSha256;
                record.sourceSize = sourceSize;
                record.conversionProfile = conversionProfile;
                record.originLookupPending = true;
                record.info.state = QStringLiteral("Refreshing");
                record.info.stage = QStringLiteral("RefreshingMedia");
                record.info.message = tr(
                    "Checking whether this media is already on the device...");
                publishOperation(operationId);
                emit requestBeginPrinterForegroundOperation(operationId,
                                                             generation);
                emit requestPrinterRefreshMedia(currentPrinterPath(),
                                                operationId, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::progress, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.state = QStringLiteral("Converting");
                record.info.stage = QStringLiteral("Converting");
                record.info.message = message;
                publishOperation(operationId);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::failed, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                const QString category =
                    operations_[operationId].info.stage ==
                            QStringLiteral("HashingSource")
                        ? QStringLiteral("SourceAnalysisFailed")
                        : QStringLiteral("ConversionFailed");
                finishOperation(operationId, QStringLiteral("Failed"),
                                category,
                                QString(), message);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::prepared, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &devicePath,
                                           const QString &sourcePath,
                                           const QString &uploadPath,
                                           const QString &remoteName,
                                           const QString &preparedSha256,
                                           const QString &stagedThumbnailPath,
                                           const QString &stagedThumbnailSha256,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    currentPrinterPath() != devicePath ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    releasePrinterPreparationPath(uploadPath);
                    releasePrinterPreparationPath(
                        stagedThumbnailPath);
                    auto staleRecord = operations_.find(operationId);
                    if (staleRecord != operations_.end() &&
                        staleRecord->sourcePath == sourcePath) {
                        releaseOwnedSource(*staleRecord);
                    }
                    return;
                }
                OperationRecord &record = operations_[operationId];
                const QString completedFingerprint =
                    sourceFingerprint(sourcePath);
                if (completedFingerprint.isEmpty() ||
                    completedFingerprint != record.sourceFingerprint) {
                    releasePrinterPreparationPath(uploadPath);
                    releasePrinterPreparationPath(
                        stagedThumbnailPath);
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceChanged"), QString(),
                        tr("Source media changed while it was being converted"));
                    return;
                }
                record.sourcePath = sourcePath;
                record.preparedPath = uploadPath;
                record.preparedSha256 = preparedSha256;
                record.stagedThumbnailPath = stagedThumbnailPath;
                record.stagedThumbnailSha256 = stagedThumbnailSha256;
                record.remoteName = remoteName;
                record.originalRemoteName = remoteName;
                releaseOwnedSource(record);
                record.info.resultName = remoteName;
                record.info.total = QFileInfo(uploadPath).size();
                record.info.state = QStringLiteral("Preflight");
                record.info.stage = QStringLiteral("EnsuringSession");
                record.info.message = tr("Prepared media is ready for upload");
                if (record.replaceOperation) {
                    QString journalError;
                    if (!writeReplaceJournal(
                            operationId,
                            QStringLiteral("Uploading"),
                            &journalError)) {
                        removePreparedFileForOperation(operationId);
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral(
                                "ReplaceJournalWriteFailed"),
                            QString(),
                            tr("Replacement was stopped before upload because its journal could not be persisted: %1")
                                .arg(journalError));
                        return;
                    }
                }
                publishOperation(operationId);
                emit requestBeginPrinterForegroundOperation(operationId,
                                                             generation);
                dispatchPreparedUploadWithRetryBarrier(
                    devicePath, operationId, generation);
            });

    connect(keepaliveTimer_, &QTimer::timeout, this, [this]() {
        if (!runtimeDowngradeV10Prepared_ &&
            !firmwareExclusiveActive() &&
            !firmwareRecoveryInterlockActive_ &&
            !printerClassConnected_) {
            emit requestKeepalive();
        }
    });

    connect(printerMonitor_, &PrinterDeviceMonitor::snapshotChanged,
            this, &DeviceManager::handlePrinterSnapshot);
    connect(printerMonitor_, &PrinterDeviceMonitor::currentEndpointRemoved,
            this, [this]() {
                ++printerDisconnectCount_;
                const QString sysfsPath =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().sysfsPath
                        : QString();
                const QString serial =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().serial.trimmed()
                        : printerDeviceSerial_.trimmed();
                logPrinterLifecycleEvent(
                    QStringLiteral("endpoint_removed"),
                    printerGeneration_,
                    {
                        {QStringLiteral("sysfs_path"), sysfsPath},
                        {QStringLiteral("serial"), serial},
                        {QStringLiteral("disconnect_count"),
                         QString::number(printerDisconnectCount_)},
                        {QStringLiteral("lease_mode"),
                         printerOverlayLeaseModeName(
                             printerOverlayLeaseMode_)}
                    });
                if (printerRecoveryRequired_ &&
                    isPrinterClassDevicePresent()) {
                    printerRecoveryRemovalObserved_ = true;
                }
                if (printerDisplaySessionLost_ &&
                    isPrinterClassDevicePresent()) {
                    printerSessionLossRemovalObserved_ = true;
                }
            });
    connect(printerMonitor_, &PrinterDeviceMonitor::monitorError,
            this, &DeviceManager::deviceError);

    printerPreparationThread_.start();
    workerThread_.start();
    if (startPrinterMonitor) {
        // The metadata scan is bounded and does not hash media. Reserve the
        // scheduler before the D-Bus service can accept foreground work; the
        // potentially large SHA-256 validation remains queued on the
        // preparation thread.
        cleanupMediaRuntimeStaging();
        cleanupDeviceMediaOutbox();
        loadMediaCatalogStore();
        loadSavedLayoutsStore();
        loadPaseMetricsConfig();
        loadRetryCache();
        loadReplaceJournal();
        loadDeleteIntent();
        artifactSweepTimer_->start();
        printerMonitor_->start();
    }
}

#ifdef TRYX_PROTOCOL_TESTING
DeviceManager *DeviceManager::createForTesting(
    const QString &sysfsRoot, const QString &devRoot, QObject *parent) {
    auto *monitor = new PrinterDeviceMonitor;
    monitor->setDiscoveryRootsForTesting(sysfsRoot, devRoot);
    auto *manager = new DeviceManager(monitor, false, parent);
    manager->retryCacheDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("retry-cache"));
    manager->mediaCatalogStore_ =
        std::make_unique<tryx::MediaCatalogStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("media-catalog")));
    manager->paseMetricsConfigStore_ =
        std::make_unique<tryx::PaseMetricsConfigStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("pase-config")));
    manager->runtimePresentationPreferencesStore_ =
        std::make_unique<tryx::RuntimePresentationPreferencesStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral(
                    "runtime-presentation-preferences")));
    manager->savedLayoutStore_ =
        std::make_unique<tryx::SavedLayoutStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("saved-layouts")));
    manager->runtimeDowngradeStore_ =
        std::make_unique<tryx::RuntimeDowngradeStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral(
                    "runtime-downgrade")));
    manager->loadRuntimePresentationPreferences();
    manager->loadSavedLayoutsStore();
    manager->mediaRuntimeRootOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("runtime-staging"));
    manager->deviceMediaArtifactStore_ =
        std::make_unique<tryx::DeviceMediaArtifactStore>(
            QDir(manager->mediaRuntimeRootOverride_)
                .filePath(QStringLiteral("device-media-outbox")));
    manager->cleanupDeviceMediaOutbox();
    manager->artifactSweepTimer_->start();
    manager->loadMediaCatalogStore();
    manager->loadRetryCache();
    return manager;
}

void DeviceManager::setAutoConnectModeForTesting(bool enabled) {
    autoConnectMode_ = enabled;
}

void DeviceManager::rescanPrinterForTesting() {
    printerMonitor_->rescanForTesting(false);
}

void DeviceManager::injectPrinterUdevEventForTesting(
    const QByteArray &subsystem, const QString &syspath,
    const QString &sysname) {
    printerMonitor_->injectUdevEventForTesting(subsystem, syspath, sysname);
}

quint64 DeviceManager::printerGenerationForTesting() const {
    return printerGeneration_;
}

bool DeviceManager::printerDisplaySessionActiveForTesting() const {
    return printerDisplaySessionActive_;
}

bool DeviceManager::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    const bool adopted = QMetaObject::invokeMethod(
        worker_,
        [this, fd, devicePath]() {
            worker_->adoptPrinterFileDescriptorForTesting(fd, devicePath);
        },
        Qt::BlockingQueuedConnection);
    if (adopted && printerClassConnected_ &&
        devicePath == currentPrinterPath() &&
        !retryCacheMutationGateActive()) {
        emit requestStartPrinterSession(devicePath, printerGeneration_);
        if (!automaticPrinterSessionStart_) {
            QMetaObject::invokeMethod(
                worker_,
                [this, devicePath, generation = printerGeneration_]() {
                    worker_->startPrinterDisplaySession(devicePath, generation);
                },
                Qt::QueuedConnection);
        }
    }
    return adopted;
}

void DeviceManager::emitPrinterDeviceInfoFailureForTesting(
    const QString &message, quint64 generation) {
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, message, generation]() {
            emit worker->printerDeviceInfoFailed(message, generation);
        },
        Qt::QueuedConnection);
}

void DeviceManager::emitPrinterSessionLostForTesting(quint64 generation) {
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            emit worker->printerSessionLost(generation);
        },
        Qt::QueuedConnection);
}
#endif

DeviceManager::~DeviceManager() {
    if (artifactSweepTimer_) {
        artifactSweepTimer_->stop();
    }
    for (auto validation = pendingRetryCacheValidations_.cbegin();
         validation != pendingRetryCacheValidations_.cend(); ++validation) {
        printerMediaPreparer_->cancelRetryValidation(validation.key());
    }
    pendingRetryCacheValidations_.clear();
    disconnect(printerMonitor_, nullptr, this, nullptr);
    stopKeepalive();
    cancelForegroundForGenerationChange(
        tr("TRYX runtime is stopping"));
    clearDeviceSpecificationsCache();
    ++printerGeneration_;
    emit requestCancelPrinterPreparation(printerGeneration_);
    worker_->updatePrinterGenerationGate(printerGeneration_, false);
    emit requestClearPrinter(printerGeneration_);
    emit requestDisconnect();
    if (printerPreparationThread_.isRunning()) {
        QMetaObject::invokeMethod(printerMediaPreparer_, "shutdown",
                                  Qt::BlockingQueuedConnection);
        printerPreparationThread_.quit();
        printerPreparationThread_.wait();
    }
    workerThread_.quit();
    workerThread_.wait();
    for (auto record = operations_.begin();
         record != operations_.end(); ++record) {
        releaseOwnedSource(*record);
    }
    deviceMediaArtifactStore_->clearAfterWorkersStopped();
}

void DeviceManager::setPrinterDisplaySessionActive(bool active) {
    if (printerDisplaySessionActive_ == active) {
        return;
    }
    printerDisplaySessionActive_ = active;
    emit printerDisplaySessionChanged(active);
}

void DeviceManager::clearDeviceSpecificationsCache() {
    deviceSpecificationsCache_ = {};
    deviceSpecificationsDevicePath_.clear();
    deviceSpecificationsDeviceIdentity_.clear();
    deviceSpecificationsProductId_ = 0;
    deviceSpecificationsGeneration_ = 0;
}

void DeviceManager::handlePrinterSnapshot(
    const PrinterProtocol::DiscoverySnapshot &snapshot) {
    if (runtimeDowngradeV10Prepared_) {
        return;
    }
    const bool oldPresence = isPrinterClassDevicePresent();
    const bool wasConnected = connected_;
    const bool wasPrinterConnected = printerClassConnected_;
    const QString oldPath = printerDevicePath_;
    const QString oldSerial = printerDeviceSerial_;
    const quint16 oldProductId = printerProductId_;

    if (printerRecoveryRequired_ &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        printerRecoveryRemovalObserved_ = true;
    }
    if (printerDisplaySessionLost_ &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        printerSessionLossRemovalObserved_ = true;
    }

    if (wasPrinterConnected && printerDisplaySessionActive_ &&
        !oldSerial.isEmpty()) {
        printerSessionResumePending_ = true;
        printerSessionResumeSerial_ = oldSerial;
        printerSessionResumeProductId_ = oldProductId;
    }
    setPrinterDisplaySessionActive(false);
    clearDeviceSpecificationsCache();

    cancelForegroundForGenerationChange(
        tr("Printer-class operation stopped because the USB connection changed"));
    printerSnapshot_ = snapshot;
    ++printerGeneration_;
    printerGenerationElapsedTimer_.start();
    emit requestCancelPrinterPreparation(printerGeneration_);
    if (wasPrinterConnected) {
        emit printerOperationsCancelled();
    }
    if (firmwareExclusiveActive() ||
        firmwareRecoveryInterlockActive_) {
        worker_->updatePrinterGenerationGate(
            printerGeneration_, false);
        detachPrinterClassDevice(false);
        legacyProductId_.clear();
        connected_ = false;
        printerSessionResumePending_ = false;
        printerSessionResumeSerial_.clear();
        printerSessionResumeProductId_ = 0;
        emit mediaListUpdated({});
        if (wasConnected) {
            emit deviceDisconnected();
        }
        const bool newPresence =
            isPrinterClassDevicePresent();
        if (oldPresence != newPresence) {
            emit printerPresenceChanged(newPresence);
        }
        return;
    }
    const bool ready = snapshot.state == PrinterProtocol::DiscoveryState::Ready &&
                       snapshot.devices.size() == 1;
    const bool endpointSelected = ready &&
                                  (autoConnectMode_ || wasPrinterConnected);
    const QString snapshotSysfsPath = snapshot.devices.size() == 1
        ? snapshot.devices.first().sysfsPath
        : QString();
    const QString snapshotSerial = snapshot.devices.size() == 1
        ? snapshot.devices.first().serial.trimmed()
        : QString();
    logPrinterLifecycleEvent(
        QStringLiteral("physical_generation_changed"),
        printerGeneration_,
        {
            {QStringLiteral("discovery_state"),
             printerDiscoveryStateName(snapshot.state)},
            {QStringLiteral("sysfs_path"), snapshotSysfsPath},
            {QStringLiteral("serial"), snapshotSerial},
            {QStringLiteral("endpoint_selected"),
             endpointSelected ? QStringLiteral("true")
                              : QStringLiteral("false")},
            {QStringLiteral("disconnect_count"),
             QString::number(printerDisconnectCount_)},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(
                 printerOverlayLeaseMode_)}
        });
    if (ready) {
        logPrinterLifecycleEvent(
            QStringLiteral("endpoint_discovered"),
            printerGeneration_,
            {
                {QStringLiteral("sysfs_path"),
                 snapshotSysfsPath},
                {QStringLiteral("serial"), snapshotSerial},
                {QStringLiteral("endpoint_selected"),
                 endpointSelected ? QStringLiteral("true")
                                  : QStringLiteral("false")},
                {QStringLiteral("disconnect_count"),
                 QString::number(printerDisconnectCount_)}
            });
    }
    const bool retryCacheValidationComplete =
        !retryCacheStartupSessionGateActive();
    const bool sessionLossAllowsSession =
        !printerDisplaySessionLost_ ||
        printerSessionLossRemovalObserved_;
    const bool recoveryAllowsEndpoint =
        retryCacheValidationComplete &&
        sessionLossAllowsSession &&
        (!printerRecoveryRequired_ ||
         (endpointSelected &&
          completePrinterRecoveryAfterRemoval(
              snapshot.devices.first().serial,
              snapshot.devices.first().productId)));
    const bool restrictedRecoverySession =
        recoveryAllowsEndpoint &&
        retryCacheRestrictedRecoveryActive();
    const bool recoveryAllowsSession =
        recoveryAllowsEndpoint &&
        !restrictedRecoverySession;
    const bool resumeSelectedSession =
        endpointSelected && printerSessionResumePending_ &&
        !snapshot.devices.first().serial.isEmpty() &&
        snapshot.devices.first().serial == printerSessionResumeSerial_ &&
        snapshot.devices.first().productId ==
            printerSessionResumeProductId_;
    if (ready && printerSessionResumePending_ && !resumeSelectedSession) {
        printerSessionResumePending_ = false;
        printerSessionResumeSerial_.clear();
        printerSessionResumeProductId_ = 0;
    }
    worker_->updatePrinterGenerationGate(
        printerGeneration_, endpointSelected &&
            (recoveryAllowsSession || restrictedRecoverySession));

    if (endpointSelected) {
        const QString newPath = snapshot.devices.first().devicePath;
        const QString newSerial = snapshot.devices.first().serial;
        const quint16 newProductId = snapshot.devices.first().productId;
        if (wasPrinterConnected &&
            (oldPath != newPath || oldSerial != newSerial ||
             oldProductId != newProductId)) {
            detachPrinterClassDevice(true);
        } else if (wasPrinterConnected) {
            emit printerDeviceVersionsReady(QString(), QString());
        }
        emit requestConfigurePrinter(
            newPath, newSerial, newProductId, printerGeneration_);
        const std::optional<PrinterProductProfile> productProfile =
            printerProductProfileForId(newProductId);
        const PrinterProtocol::PaseOverlayConfig restoredOverlay =
            productProfile && productProfile->overlayMetricsSupported
                ? persistedPaseOverlayForDevice(newSerial)
                : PrinterProtocol::PaseOverlayConfig{};
        metricsState_.deviceSerial = newSerial.trimmed();
        metricsState_.enabled = paseOverlayHasMetrics(restoredOverlay);
        metricsState_.samplingActive = false;
        metricsState_.metrics = restoredOverlay.left.metrics;
        metricsState_.alignment = restoredOverlay.left.alignment;
        metricsState_.textColor = restoredOverlay.left.textColor;
        metricsState_.availableMetrics.clear();
        metricsState_.diagnostic.clear();
        publishMetricsState();
        if (recoveryAllowsSession &&
            productProfile && productProfile->overlayMetricsSupported &&
            paseOverlayHasContent(restoredOverlay)) {
            emit requestRestorePrinterOverlay(
                restoredOverlay, printerGeneration_);
        }
        if (resumeSelectedSession) {
            emit uploadStatus(
                tr("Restoring the active PASE display session after USB re-enumeration..."));
        }
        if (recoveryAllowsSession) {
            printerDisplaySessionLost_ = false;
            printerSessionLossRemovalObserved_ = false;
            emit requestStartPrinterSession(newPath, printerGeneration_);
        } else if (!retryCacheValidationComplete &&
                   !printerDisplaySessionLost_) {
            printerDisplaySessionLost_ = false;
            emit uploadStatus(tr(
                "Stored retry media is still being validated; the PASE display session will start only after validation finishes"));
        } else {
            printerDisplaySessionLost_ =
                !restrictedRecoverySession;
            emit uploadStatus(printerMutationUnavailableStatusText());
        }
    } else {
        emit requestClearPrinter(printerGeneration_);
        if (wasPrinterConnected) {
            detachPrinterClassDevice(true);
        }
    }
    if (wasPrinterConnected) {
        emit mediaListUpdated({});
    }

    if (snapshot.blocksLegacyTransport() && connected_ &&
        !printerClassConnected_) {
        legacyProductId_.clear();
        connected_ = false;
        emit mediaListUpdated({});
        emit requestDisconnect();
    }

    const bool newPresence = isPrinterClassDevicePresent();
    if (oldPresence != newPresence) {
        emit printerPresenceChanged(newPresence);
    }

    if (!autoConnectMode_) {
        return;
    }

    switch (snapshot.state) {
    case PrinterProtocol::DiscoveryState::Ready:
        if (!printerClassConnected_ && snapshot.devices.size() == 1) {
            attachPrinterClassDevice(snapshot.devices.first());
        }
        startRetryCacheReadOnlyReconciliationIfReady();
        break;
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
    case PrinterProtocol::DiscoveryState::EnumeratingPrinterClass:
        emit uploadStatus(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::PermissionDenied:
    case PrinterProtocol::DiscoveryState::Ambiguous:
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        emit deviceError(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::Absent:
        if (!connected_) {
            const auto legacyPort = panorama::Device::find_device();
            if (legacyPort) {
                emit requestConnect(QString::fromStdString(*legacyPort));
            } else {
                emit uploadStatus(
                    tr("Waiting for TRYX device. Reconnect USB or keep Auto connection selected."));
            }
        }
        break;
    }

}

void DeviceManager::connectDevice(const QString &port) {
    if (runtimeDowngradeV10Prepared_) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (firmwareRecoveryInterlockActive_) {
        emit deviceError(tr(
            "Device connection is blocked until firmware recovery is explicitly acknowledged"));
        return;
    }
    if (!port.isEmpty() && printerSnapshot_.blocksLegacyTransport()) {
        emit deviceError(
            tr("A TRYX printer-class or Rockchip gadget device is present; use Auto connection."));
        return;
    }
    setPrinterDisplaySessionActive(false);
    clearDeviceSpecificationsCache();
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    printerSessionResumeProductId_ = 0;
    autoConnectMode_ = port.isEmpty();
    const bool wasLegacyConnected =
        connected_ && !printerClassConnected_;
    legacyProductId_.clear();
    if (wasLegacyConnected) {
        connected_ = false;
        emit mediaListUpdated({});
        emit deviceDisconnected();
    }

    if (port.isEmpty()) {
        if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready &&
            printerSnapshot_.devices.size() == 1) {
            cancelForegroundForGenerationChange(
                tr("Printer-class connection was restarted"));
            ++printerGeneration_;
            printerGenerationElapsedTimer_.start();
            emit requestCancelPrinterPreparation(printerGeneration_);
            const bool retryCacheValidationComplete =
                !retryCacheStartupSessionGateActive();
            const bool sessionLossAllowsSession =
                !printerDisplaySessionLost_ ||
                printerSessionLossRemovalObserved_;
            const bool recoveryAllowsEndpoint =
                retryCacheValidationComplete &&
                sessionLossAllowsSession &&
                (!printerRecoveryRequired_ ||
                 completePrinterRecoveryAfterRemoval(
                     printerSnapshot_.devices.first().serial,
                     printerSnapshot_.devices.first().productId));
            const bool restrictedRecoverySession =
                recoveryAllowsEndpoint &&
                retryCacheRestrictedRecoveryActive();
            const bool recoveryAllowsSession =
                recoveryAllowsEndpoint &&
                !restrictedRecoverySession;
            worker_->updatePrinterGenerationGate(
                printerGeneration_, recoveryAllowsSession ||
                    restrictedRecoverySession);
            emit requestConfigurePrinter(
                printerSnapshot_.devices.first().devicePath,
                printerSnapshot_.devices.first().serial,
                printerSnapshot_.devices.first().productId,
                printerGeneration_);
            const std::optional<PrinterProductProfile> productProfile =
                printerProductProfileForId(
                    printerSnapshot_.devices.first().productId);
            const PrinterProtocol::PaseOverlayConfig restoredOverlay =
                productProfile && productProfile->overlayMetricsSupported
                    ? persistedPaseOverlayForDevice(
                          printerSnapshot_.devices.first().serial)
                    : PrinterProtocol::PaseOverlayConfig{};
            metricsState_.deviceSerial =
                printerSnapshot_.devices.first().serial.trimmed();
            metricsState_.enabled =
                paseOverlayHasMetrics(restoredOverlay);
            metricsState_.samplingActive = false;
            metricsState_.metrics = restoredOverlay.left.metrics;
            metricsState_.alignment =
                restoredOverlay.left.alignment;
            metricsState_.textColor =
                restoredOverlay.left.textColor;
            metricsState_.availableMetrics.clear();
            metricsState_.diagnostic.clear();
            publishMetricsState();
            if (recoveryAllowsSession &&
                productProfile &&
                productProfile->overlayMetricsSupported &&
                paseOverlayHasContent(restoredOverlay)) {
                emit requestRestorePrinterOverlay(
                    restoredOverlay, printerGeneration_);
            }
            attachPrinterClassDevice(printerSnapshot_.devices.first());
            if (recoveryAllowsSession) {
                printerDisplaySessionLost_ = false;
                printerSessionLossRemovalObserved_ = false;
                emit requestStartPrinterSession(
                    printerSnapshot_.devices.first().devicePath,
                    printerGeneration_);
            } else if (!retryCacheValidationComplete &&
                       !printerDisplaySessionLost_) {
                printerDisplaySessionLost_ = false;
                emit uploadStatus(tr(
                    "Stored retry media is still being validated; the PASE display session will start only after validation finishes"));
            } else {
                printerDisplaySessionLost_ =
                    !restrictedRecoverySession;
                emit uploadStatus(printerMutationUnavailableStatusText());
            }
            startRetryCacheReadOnlyReconciliationIfReady();
            return;
        }
        if (printerSnapshot_.blocksLegacyTransport()) {
            legacyProductId_.clear();
            connected_ = false;
            printerClassConnected_ = false;
            printerDevicePath_.clear();
            const QString status = printerSnapshot_.statusText();
            if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::PermissionDenied ||
                printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ambiguous ||
                printerSnapshot_.state ==
                    PrinterProtocol::DiscoveryState::MonitoringUnavailable) {
                emit deviceError(status);
            } else {
                emit uploadStatus(status);
            }
            return;
        }

        const auto legacyPort = panorama::Device::find_device();
        if (!legacyPort) {
            legacyProductId_.clear();
            connected_ = false;
            printerClassConnected_ = false;
            printerDevicePath_.clear();
            emit uploadStatus(
                tr("Waiting for TRYX device. Reconnect USB or keep Auto connection selected."));
            return;
        }
        emit requestConnect(QString::fromStdString(*legacyPort));
        return;
    }

    detachPrinterClassDevice(false);
    emit requestConnect(port);
}

void DeviceManager::disconnectDevice() {
    if (runtimeDowngradeV10Prepared_) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    autoConnectMode_ = false;
    setPrinterDisplaySessionActive(false);
    clearDeviceSpecificationsCache();
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    printerSessionResumeProductId_ = 0;
    stopKeepalive();
    const bool notifyPrinterDisconnect = printerClassConnected_;
    detachPrinterClassDevice(false);
    cancelForegroundForGenerationChange(
        tr("Printer-class operation stopped because the device was disconnected"));
    ++printerGeneration_;
    emit requestCancelPrinterPreparation(printerGeneration_);
    if (notifyPrinterDisconnect) {
        emit printerOperationsCancelled();
    }
    worker_->updatePrinterGenerationGate(printerGeneration_, false);
    emit requestClearPrinter(printerGeneration_);
    emit requestDisconnect();
    connected_ = false;
    legacyProductId_.clear();
    emit mediaListUpdated({});
    if (notifyPrinterDisconnect) {
        emit deviceDisconnected();
    }
}

void DeviceManager::requestDeviceInfo() {
    if (firmwareExclusiveActive()) {
        emit printerDeviceInfoFailed(
            firmwareExclusiveStatusText());
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (!devicePath.isEmpty()) {
        if (retryCacheMutationGateActive() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_) {
            emit printerDeviceInfoFailed(
                printerMutationUnavailableStatusText());
            return;
        }
        emit requestPrinterDeviceInfo(devicePath, printerGeneration_);
        return;
    }
    if (printerSnapshot_.blocksLegacyTransport()) {
        emit printerDeviceInfoFailed(printerSnapshot_.statusText());
        return;
    }
    if (connected_) {
        emit uploadStatus(tr("Legacy device information is available from its connection handshake."));
        return;
    }
    emit printerDeviceInfoFailed(tr("TRYX device is not connected"));
}

bool DeviceManager::isPrinterClassDevicePresent() const {
    return printerClassConnected_ || printerSnapshot_.blocksLegacyTransport();
}

void DeviceManager::attachPrinterClassDevice(
    const PrinterProtocol::UsbPrinterDevice &device) {
    if (printerClassConnected_ && printerDevicePath_ == device.devicePath &&
        printerProductId_ == device.productId) {
        return;
    }
    stopKeepalive();
    clearDeviceSpecificationsCache();
    connected_ = true;
    printerClassConnected_ = true;
    legacyProductId_.clear();
    setPrinterDisplaySessionActive(false);
    printerDevicePath_ = device.devicePath;
    printerDeviceSerial_ = device.serial;
    printerProductId_ = device.productId;
    clearMediaCatalogView();
    emit mediaListUpdated({});
    emit deviceConnected(printerProductIdString(device.productId),
                         device.serial.isEmpty() ? device.devicePath : device.serial,
                         QString(), QString());
}

void DeviceManager::detachPrinterClassDevice(bool notify) {
    const bool wasConnected = printerClassConnected_;
    clearDeviceSpecificationsCache();
    printerClassConnected_ = false;
    printerDevicePath_.clear();
    printerDeviceSerial_.clear();
    printerProductId_ = 0;
    if (wasConnected) {
        displayStateReadGeneration_ = 0;
        const quint64 metricsRevision = metricsState_.revision;
        metricsState_ = TryxRuntimeMetricsState{};
        metricsState_.revision = metricsRevision;
        publishMetricsState();
        const quint64 displayRevision = displayState_.revision;
        displayState_ = TryxRuntimeDisplayState{};
        displayState_.revision = displayRevision;
        publishDisplayState();
        connected_ = false;
        clearMediaCatalogView();
        emit mediaListUpdated({});
        if (notify) {
            emit deviceDisconnected();
        }
    }
}

QString DeviceManager::currentPrinterPath() const {
    if (!printerClassConnected_ ||
        printerSnapshot_.state != PrinterProtocol::DiscoveryState::Ready ||
        printerSnapshot_.devices.size() != 1) {
        return {};
    }
    return printerSnapshot_.devices.first().devicePath;
}

std::optional<PrinterProductProfile>
DeviceManager::currentPrinterProductProfile() const {
    quint16 productId = printerProductId_;
    if (productId == 0 &&
        printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready &&
        printerSnapshot_.devices.size() == 1) {
        productId = printerSnapshot_.devices.first().productId;
    }
    return printerProductProfileForId(productId);
}

bool DeviceManager::currentPrinterSupportsMediaCatalog() const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->mediaCatalogSupported;
}

bool DeviceManager::currentPrinterSupportsDisplayConfiguration() const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->displayConfigurationSupported;
}

bool DeviceManager::currentPrinterSupportsOverlayMetrics() const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->overlayMetricsSupported;
}

bool DeviceManager::operationMatchesCurrentPrinterProduct(
    const OperationRecord &record) const {
    const auto profile = currentPrinterProductProfile();
    return profile && record.printerProductId != 0 &&
           record.printerProductId == profile->productId;
}

bool DeviceManager::firmwareFlashAllowedForCurrentDevice(
    QString *errorMessage) const {
    const auto profile = currentPrinterProductProfile();
    if (profile && profile->firmwareFlashSupported) {
        return true;
    }
    const bool identifiedLegacyPanoramaSe =
        !profile && connected_ && !printerClassConnected_ &&
        !printerSnapshot_.blocksLegacyTransport() &&
        legacyProductId_.compare(
            QStringLiteral("cm01"), Qt::CaseInsensitive) == 0;
    if (identifiedLegacyPanoramaSe) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = profile
            ? tr("Firmware flashing is not supported for USB product %1")
                  .arg(printerProductIdString(profile->productId))
            : tr("Firmware flashing requires a connected, identified firmware-capable TRYX device");
    }
    return false;
}

QString DeviceManager::printerUnavailableStatusText() const {
    if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready &&
        printerSnapshot_.devices.size() == 1 && !printerClassConnected_) {
        return tr(
            "TRYX endpoint is present, but the display session stopped. Reconnect USB or select Auto connection again.");
    }
    return printerSnapshot_.statusText();
}

QString DeviceManager::printerMutationUnavailableStatusText() const {
    if (runtimeDowngradeV10Prepared_) {
        return tr(
            "Device mutations are blocked because runtime downgrade preparation is committed");
    }
    if (firmwareExclusiveActive()) {
        return firmwareExclusiveStatusText();
    }
    if (retryCacheStoreBlocksMutations()) {
        return tr(
            "Device mutations are blocked because the retry-cache transition is invalid or unsafe. Preserve the cache and inspect the runtime logs before retrying.");
    }
    if (!pendingRetryCacheValidations_.isEmpty() ||
        !retryCacheLoadComplete_) {
        return tr(
            "Stored retry media is still being validated; wait for validation to finish before using the PASE display session.");
    }
    if (retryCacheRestrictedRecoveryActive()) {
        return tr(
            "Device mutations are blocked while the stored upload is resolved through read-only FileList reconciliation or a proven physical reconnect.");
    }
    if (printerRecoveryRequired_) {
        return tr(
            "PASE must be power-cycled before another upload or display change. Disconnect its USB/power while the TRYX runtime is running, reconnect it, and wait for the display session to become active.");
    }
    if (printerDisplaySessionLost_) {
        return tr(
            "The PASE display session is lost. Reconnect the device and wait for a new display session before trying again.");
    }
    if (!printerDisplaySessionActive_) {
        return tr(
            "The PASE display session is not ready yet. Wait until the device finishes connecting before trying again.");
    }
    return printerUnavailableStatusText();
}

QString DeviceManager::firmwareExclusiveStatusText() const {
    return tr(
        "Device controls are unavailable while firmware flashing owns the USB transport");
}

void DeviceManager::resumePrinterSessionAfterRetryCacheValidation() {
    if (!worker_ ||
        firmwareExclusiveActive() ||
        firmwareRecoveryInterlockActive_ ||
        retryCacheMutationGateActive() ||
        printerRecoveryRequired_) {
        return;
    }
    if (printerDisplaySessionLost_ &&
        !printerSessionLossRemovalObserved_) {
        emit uploadStatus(printerMutationUnavailableStatusText());
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        return;
    }

    worker_->updatePrinterGenerationGate(printerGeneration_, true);
    const PrinterProtocol::PaseOverlayConfig restoredOverlay =
        currentPrinterSupportsOverlayMetrics()
            ? persistedPaseOverlayForDevice(printerDeviceSerial_)
            : PrinterProtocol::PaseOverlayConfig{};
    if (currentPrinterSupportsOverlayMetrics() &&
        paseOverlayHasContent(restoredOverlay)) {
        emit requestRestorePrinterOverlay(
            restoredOverlay, printerGeneration_);
    }
    printerDisplaySessionLost_ = false;
    printerSessionLossRemovalObserved_ = false;
    emit requestStartPrinterSession(devicePath, printerGeneration_);
}

void DeviceManager::requirePrinterRecovery(const QString &message) {
    const bool enteringRecovery = !printerRecoveryRequired_;
    printerRecoveryRequired_ = true;
    if (enteringRecovery) {
        printerRecoveryRemovalObserved_ =
            printerSnapshot_.state ==
                PrinterProtocol::DiscoveryState::Absent;
    }
    printerDisplaySessionLost_ = true;
    clearDeviceSpecificationsCache();
    printerSessionLossRemovalObserved_ = false;
    setPrinterDisplaySessionActive(false);
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    printerSessionResumeProductId_ = 0;
    if (worker_) {
        if (enteringRecovery) {
            ++printerGeneration_;
            emit requestCancelPrinterPreparation(printerGeneration_);
        }
        worker_->updatePrinterGenerationGate(printerGeneration_, false);
        emit requestClearPrinter(printerGeneration_);
    }
    if (!message.isEmpty()) {
        emit uploadStatus(message);
    }
}

bool DeviceManager::completePrinterRecoveryAfterRemoval(
    const QString &currentDeviceIdentity,
    quint16 currentProductId) {
    if (!printerRecoveryRequired_) {
        return true;
    }
    if (!printerRecoveryRemovalObserved_) {
        return false;
    }
    const QString retryOperationId = retryCacheVisibleOperationId();
    if (!retryOperationId.isEmpty() &&
        operations_.contains(retryOperationId)) {
        OperationRecord &record = operations_[retryOperationId];
        if (record.printerProductId == 0 ||
            record.printerProductId != currentProductId) {
            record.info.message = tr(
                "Prepared media belongs to USB product %1, but the reconnected device is %2")
                                      .arg(printerProductIdString(
                                               record.printerProductId),
                                           printerProductIdString(
                                               currentProductId));
            publishOperation(retryOperationId);
            emit deviceError(record.info.message);
            return false;
        }
        const QString observedIdentity = currentDeviceIdentity.trimmed();
        const QString expectedIdentity =
            record.uploadDeviceIdentity.trimmed();
        if (expectedIdentity.isEmpty()) {
            record.info.message = tr(
                "The original PASE identity is unavailable. Prepared media cannot be retried automatically.");
            publishOperation(retryOperationId);
            emit deviceError(record.info.message);
            return false;
        }
        if (observedIdentity.isEmpty()) {
            record.info.message = tr(
                "PASE was reconnected, but its device identity is unavailable. Retry remains blocked.");
            publishOperation(retryOperationId);
            emit deviceError(record.info.message);
            return false;
        }
        if (!expectedIdentity.isEmpty() &&
            expectedIdentity != observedIdentity) {
            record.info.message = tr(
                "A different PASE was connected after the incomplete transfer. Reconnect the original device before Retry.");
            publishOperation(retryOperationId);
            emit deviceError(record.info.message);
            return false;
        }
        tryx::RetryCacheStore::MutationResult resolved;
        bool storeResolutionRequired = false;
        bool retiresFencedDispatch = false;
        if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
            retryCacheDispatchPhaseIsRestricted(
                retryCacheSnapshot_.inFlightDispatch->phase)) {
            const auto dispatch =
                *retryCacheSnapshot_.inFlightDispatch;
            storeResolutionRequired = true;
            retiresFencedDispatch = true;
            resolved = retryCacheStore().resolveShadowMissingFence(
                retryCacheSnapshot_,
                retryCacheExpectedDispatch(dispatch),
                tryx::RetryCacheStore::RecoveryFenceProof::
                    PhysicalReconnectObserved);
        } else if (retryCacheSnapshot_.retryCandidate.has_value() &&
                   retryCacheSnapshot_.retryCandidate
                       ->requiresDeviceRecovery &&
                   !retryCacheSnapshot_.retryCandidate
                       ->finalizationOnlyReconciliation) {
            const auto candidate =
                *retryCacheSnapshot_.retryCandidate;
            storeResolutionRequired = true;
            resolved = retryCacheStore().resolveCandidateRecovery(
                retryCacheSnapshot_,
                retryCacheExpectedDispatch(candidate),
                tryx::RetryCacheStore::CandidateRecoveryProof::
                    PhysicalReconnectObserved);
        }
        if (storeResolutionRequired &&
            (!resolved.ok() || !resolved.snapshot.has_value())) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = resolved.detail;
            emit deviceError(
                tr("PASE reconnected, but the recovery state could not be saved: %1")
                    .arg(resolved.detail));
            return false;
        }
        if (storeResolutionRequired) {
            retryCacheSnapshot_ = *resolved.snapshot;
            synchronizeRetryCacheSurface();
        }
        const QString resolvedRetryOperationId =
            retryCacheVisibleOperationId();
        if (retiresFencedDispatch &&
            retryOperationId != resolvedRetryOperationId) {
            removePreparedFileForOperation(retryOperationId);
            finishOperation(
                retryOperationId, QStringLiteral("Failed"),
                QStringLiteral("ProvenNotStarted"), QString(),
                tr("The fenced dispatch was retired after a proven physical reconnect"));
            synchronizeRetryCacheSurface();
        }
        auto updated = operations_.find(resolvedRetryOperationId);
        if (updated != operations_.end()) {
            updated->uploadDeviceIdentity = observedIdentity;
            updated->info.message = tr(
                "The same PASE was physically reconnected after the incomplete transfer. Prepared media can now be transferred again under a new device filename.");
            publishOperation(resolvedRetryOperationId);
        }
    }

    printerRecoveryRequired_ = false;
    printerRecoveryRemovalObserved_ = false;
    printerDisplaySessionLost_ = false;
    printerSessionLossRemovalObserved_ = false;
    emit uploadStatus(
        tr("PASE power-cycle was observed; starting a clean display session"));
    return true;
}

QString DeviceManager::normalizedOperationId(const QString &requestedId) const {
    const QString trimmed = requestedId.trimmed();
    if (trimmed.isEmpty()) {
        return QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    }
    const QUuid parsed(trimmed);
    if (!parsed.isNull()) {
        return parsed.toString(QUuid::WithoutBraces);
    }
    return {};
}

QString DeviceManager::mediaInboxDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!mediaRuntimeRootOverride_.isEmpty()) {
        return QDir(mediaRuntimeRootOverride_)
            .filePath(QStringLiteral("media-inbox"));
    }
#endif
    return tryxRuntimeMediaInboxPath();
}

QString DeviceManager::mediaSpoolDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!mediaRuntimeRootOverride_.isEmpty()) {
        return QDir(mediaRuntimeRootOverride_)
            .filePath(QStringLiteral("media-spool"));
    }
#endif
    return tryxRuntimeMediaSpoolPath();
}

bool DeviceManager::ensureMediaRuntimeDirectories(
    QString *errorMessage) const {
    const QString inbox = mediaInboxDirectory();
    const QString spool = mediaSpoolDirectory();
    if (inbox.isEmpty() || spool.isEmpty() ||
        QFileInfo(inbox).absolutePath() !=
            QFileInfo(spool).absolutePath()) {
        if (errorMessage) {
            *errorMessage = tr(
                "The shared media staging directories are unavailable");
        }
        return false;
    }

    const QString applicationRoot = QFileInfo(inbox).absolutePath();
    const QString runtimeRoot = QFileInfo(applicationRoot).absolutePath();
    return ensurePrivateDirectory(runtimeRoot, false, errorMessage) &&
           ensurePrivateDirectory(applicationRoot, true, errorMessage) &&
           ensurePrivateDirectory(inbox, true, errorMessage) &&
           ensurePrivateDirectory(spool, true, errorMessage);
}

bool DeviceManager::claimQuickStagedSource(
    const QString &operationId, const QString &sourcePath,
    QString *claimedPath, bool *owned,
    QString *errorMessage) const {
    if (!claimedPath || !owned) {
        if (errorMessage) {
            *errorMessage = tr(
                "The staged source ownership destination is unavailable");
        }
        return false;
    }

    *claimedPath = cleanAbsolutePath(sourcePath);
    *owned = false;
    const QString inbox = mediaInboxDirectory();
    const QString spool = mediaSpoolDirectory();
    if (inbox.isEmpty() || spool.isEmpty()) {
        return true;
    }
    const QString managedRoot = QFileInfo(inbox).absolutePath();
    const QString canonicalSource =
        QFileInfo(sourcePath).canonicalFilePath();
    const bool managedPath =
        pathIsInside(sourcePath, managedRoot) ||
        (!canonicalSource.isEmpty() &&
         pathIsInside(canonicalSource, managedRoot));
    if (!managedPath) {
        return true;
    }

    QString directoryError;
    if (!ensureMediaRuntimeDirectories(&directoryError)) {
        if (errorMessage) {
            *errorMessage = directoryError;
        }
        return false;
    }

    const QFileInfo sourceInfo(*claimedPath);
    if (cleanAbsolutePath(sourceInfo.absolutePath()) !=
            cleanAbsolutePath(inbox) ||
        !stagedSourceFileNameIsValid(sourceInfo.fileName())) {
        if (errorMessage) {
            *errorMessage = tr(
                "Staged media must be one validated direct child of the shared inbox");
        }
        return false;
    }

    const QByteArray encodedSource =
        QFile::encodeName(*claimedPath);
    struct stat before {};
    if (::lstat(encodedSource.constData(), &before) != 0 ||
        !stagedSourceStatIsValid(before)) {
        if (errorMessage) {
            *errorMessage = tr(
                "Staged media must be a non-linked regular file owned by this user with mode 0600 and a supported size");
        }
        return false;
    }

    const QString suffix = sourceInfo.suffix();
    const QString destination =
        QDir(spool).filePath(operationId + QLatin1Char('.') + suffix);
    QString renameError;
    if (!atomicRenameNoReplace(
            *claimedPath, destination, &renameError)) {
        if (errorMessage) {
            *errorMessage = renameError;
        }
        return false;
    }

    const QByteArray encodedDestination =
        QFile::encodeName(destination);
    struct stat after {};
    const bool sameValidatedFile =
        ::lstat(encodedDestination.constData(), &after) == 0 &&
        stagedSourceStatIsValid(after) &&
        before.st_dev == after.st_dev &&
        before.st_ino == after.st_ino &&
        before.st_size == after.st_size &&
        before.st_uid == after.st_uid &&
        (before.st_mode & 07777) == (after.st_mode & 07777);
    if (!sameValidatedFile) {
        QString rollbackError;
        if (!atomicRenameNoReplace(
                destination, *claimedPath, &rollbackError)) {
            ::unlink(encodedDestination.constData());
        }
        if (errorMessage) {
            *errorMessage = tr(
                "The staged media identity changed while daemon ownership was acquired");
        }
        return false;
    }

    *claimedPath = destination;
    *owned = true;
    return true;
}

void DeviceManager::releaseOwnedSource(
    OperationRecord &record) {
    if (!record.ownsSourcePath) {
        return;
    }
    const QString sourcePath = cleanAbsolutePath(record.sourcePath);
    const QString spool = cleanAbsolutePath(mediaSpoolDirectory());
    if (cleanAbsolutePath(QFileInfo(sourcePath).absolutePath()) == spool) {
        const QByteArray encoded = QFile::encodeName(sourcePath);
        if (::unlink(encoded.constData()) != 0 && errno != ENOENT) {
            qWarning().noquote()
                << tr("Could not remove daemon-owned staged source %1: %2")
                       .arg(sourcePath,
                            QString::fromLocal8Bit(std::strerror(errno)));
        }
    } else {
        qWarning().noquote()
            << tr("Refusing to remove an owned source outside the daemon spool: %1")
                   .arg(sourcePath);
    }
    record.ownsSourcePath = false;
}

void DeviceManager::cleanupMediaRuntimeStaging() {
    QString directoryError;
    if (!ensureMediaRuntimeDirectories(&directoryError)) {
        qWarning().noquote()
            << tr("Could not initialize media staging: %1")
                   .arg(directoryError);
        return;
    }

    const auto sweep = [](const QString &directory,
                          bool removeAll) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QFileInfoList entries = QDir(directory).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System |
                QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QByteArray encoded =
                QFile::encodeName(entry.absoluteFilePath());
            struct stat status {};
            if (::lstat(encoded.constData(), &status) != 0 ||
                (!S_ISREG(status.st_mode) &&
                 !S_ISLNK(status.st_mode)) ||
                status.st_uid != ::geteuid()) {
                continue;
            }
            const bool aged =
                status.st_mtim.tv_sec <=
                now - kMediaInboxMaxAgeSeconds;
            if (removeAll || aged) {
                ::unlink(encoded.constData());
            }
        }
    };

    sweep(mediaSpoolDirectory(), true);
    sweep(mediaInboxDirectory(), false);
}

void DeviceManager::cleanupDeviceMediaOutbox() {
    const auto cleanup = deviceMediaArtifactStore_->initialize();
    if (!cleanup.ok()) {
        qWarning().noquote()
            << tr("Could not initialize the device media outbox: %1")
                   .arg(cleanup.result.detail);
        return;
    }
    if (!cleanup.complete) {
        qInfo()
            << "Device media outbox cleanup will continue in bounded timer batches";
    }
}

const TryxRuntimeMediaEntry *DeviceManager::findMediaById(
    const QString &mediaId) const {
    if (!isSha256Hex(mediaId)) {
        return nullptr;
    }
    const auto found = std::find_if(
        mediaCatalog_.entries.cbegin(),
        mediaCatalog_.entries.cend(),
        [&mediaId](const TryxRuntimeMediaEntry &entry) {
            return entry.mediaId == mediaId;
        });
    return found == mediaCatalog_.entries.cend()
        ? nullptr
        : &(*found);
}

bool DeviceManager::watchArtifactOwner(
    const QString &ownerUniqueName) {
    if (!artifactOwnerWatcher_ ||
        !tryx::DeviceMediaArtifactStore::isValidDbusUniqueName(
            ownerUniqueName)) {
        return false;
    }
    if (!artifactOwnerWatcher_->watchedServices().contains(
            ownerUniqueName)) {
        artifactOwnerWatcher_->addWatchedService(ownerUniqueName);
    }
    QDBusConnectionInterface *interface =
        QDBusConnection::sessionBus().interface();
    if (!interface) {
        return false;
    }
    const QDBusReply<bool> registered =
        interface->isServiceRegistered(ownerUniqueName);
    return registered.isValid() && registered.value();
}

void DeviceManager::handleArtifactOwnerUnregistered(
    const QString &ownerUniqueName) {
    if (runtimeDowngradeV10Prepared_) {
        return;
    }
    const auto disconnected =
        deviceMediaArtifactStore_->ownerDisconnected(
            ownerUniqueName,
            cacheCleanupExclusiveActive_
                ? tryx::DeviceMediaArtifactStore::OwnerDisconnectMode::
                      RevokeAndDefer
                : tryx::DeviceMediaArtifactStore::OwnerDisconnectMode::
                      RemoveIdle);
    for (const QString &operationId :
         disconnected.operationIdsToCancel) {
        cancelOperation(operationId);
    }
    if (artifactOwnerWatcher_ &&
        !deviceMediaArtifactStore_->ownerHasArtifacts(
            ownerUniqueName)) {
        artifactOwnerWatcher_->removeWatchedService(
            ownerUniqueName);
    }
}

void DeviceManager::sweepDeviceMediaArtifacts() {
    if (runtimeDowngradeV10Prepared_ || cacheCleanupExclusiveActive_) {
        return;
    }
    const auto cleanup =
        deviceMediaArtifactStore_->continueStartupCleanup();
    if (!cleanup.ok()) {
        qWarning().noquote()
            << tr("Could not continue device media outbox cleanup: %1")
                   .arg(cleanup.result.detail);
    }
    const auto swept = deviceMediaArtifactStore_->sweepExpired();
    if (!swept.ok()) {
        qWarning().noquote()
            << tr("Could not remove an expired device media artifact: %1")
                   .arg(swept.result.detail);
    }
    if (artifactOwnerWatcher_) {
        for (const QString &owner : swept.ownersNoLongerUsed) {
            artifactOwnerWatcher_->removeWatchedService(owner);
        }
    }
}

void DeviceManager::releaseArtifactOperationHold(
    const QString &operationId) {
    const auto operation = operations_.constFind(operationId);
    if (operation == operations_.constEnd() ||
        operation->artifactId.isEmpty()) {
        return;
    }
    const QString artifactId = operation->artifactId;
    const auto released =
        deviceMediaArtifactStore_->releaseOperationHold(
            artifactId, operationId);
    if (released.removed && artifactOwnerWatcher_ &&
        !released.ownerUniqueName.isEmpty() &&
        !released.ownerStillUsed) {
        artifactOwnerWatcher_->removeWatchedService(
            released.ownerUniqueName);
    }
}

bool DeviceManager::operationIsTerminal(const QString &state) const {
    return state == QStringLiteral("Succeeded") ||
           state == QStringLiteral("Failed") ||
           state == QStringLiteral("Cancelled") ||
           state == QStringLiteral("RetryAvailable");
}

TryxRuntimeOperationsSnapshot DeviceManager::operationSnapshot() const {
    TryxRuntimeOperationsSnapshot snapshot;
    snapshot.revision = operationRevision_;
    snapshot.activeOperationId = activeOperationId_;
    for (const QString &operationId : operationOrder_) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd()) {
            snapshot.operations.append(found->info);
        }
    }
    return snapshot;
}

bool DeviceManager::prepareRuntimeDowngradeV10(
    QString *mode, QString *errorName, QString *errorMessage) {
    if (mode) {
        mode->clear();
    }
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto setFailure = [errorName, errorMessage](
                                const QString &message) {
        if (errorName) {
            *errorName = QStringLiteral(
                "org.tryx.Panorama.Error.DowngradeV10Blocked");
        }
        if (errorMessage) {
            *errorMessage = message;
        }
    };
    if (QThread::currentThread() != thread()) {
        setFailure(tr(
            "Runtime downgrade preparation must run on the runtime thread"));
        return false;
    }
    if (runtimeDowngradeV10Prepared_) {
        if (runtimeDowngradeV10Mode_.isEmpty()) {
            setFailure(tr(
                "Runtime downgrade preparation is already in progress"));
            return false;
        }
        if (mode) {
            *mode = runtimeDowngradeV10Mode_;
        }
        return true;
    }

    // This latch is published before inspecting any mutable runtime state.
    // All device entry points execute on this thread, so no later request can
    // enter between the quiescence check and the durable marker commit.
    runtimeDowngradeV10Prepared_ = true;
    const auto fail = [this, &setFailure](const QString &message) {
        runtimeDowngradeV10Mode_.clear();
        runtimeDowngradeV10Prepared_ = false;
        setFailure(message);
        return false;
    };

    if (firmwareExclusiveActive() ||
        !firmwareReleasePendingLeaseId_.isEmpty() ||
        firmwareRecoveryInterlockActive_) {
        return fail(tr(
            "Firmware activity or recovery must finish before preparing a runtime downgrade"));
    }
    if (!activeOperationId_.isEmpty()) {
        return fail(tr(
            "A device operation is still active; wait for it to finish before preparing a runtime downgrade"));
    }
    for (auto operation = operations_.cbegin();
         operation != operations_.cend(); ++operation) {
        if (!operationIsTerminal(operation->info.state)) {
            return fail(tr(
                "A device operation is still pending; wait for it to finish before preparing a runtime downgrade"));
        }
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        pendingDeleteIntent_.has_value() ||
        QFileInfo::exists(deleteIntentPath()) ||
        !pendingReplaceJournalOperationId_.isEmpty() ||
        QFileInfo::exists(replaceIntentPath())) {
        return fail(tr(
            "Delete or replacement recovery must finish before preparing a runtime downgrade"));
    }
    if (!retryCacheLoadComplete_ ||
        !pendingRetryCacheValidations_.isEmpty() ||
        retryCacheStartupFailure_ || !retryCacheStore_ ||
        retryCacheStore_->blocksMutations()) {
        return fail(tr(
            "Stored retry media is not fully validated for runtime downgrade"));
    }

    const auto safety =
        retryCacheStore_->releasedV10DowngradeSafety(
            retryCacheSnapshot_);
    if (!safety.safe) {
        qWarning().noquote()
            << "Runtime downgrade v10 preparation blocked:"
            << safety.status;
        return fail(tr(
            "Stored retry media is not exactly compatible with runtime v10"));
    }
    const QString compatibilityMode =
        safety.status == QStringLiteral("SafeEmpty")
        ? QStringLiteral("Empty")
        : safety.status == QStringLiteral("SafeFullRetry")
            ? QStringLiteral("FullFrame")
            : QString();
    if (compatibilityMode.isEmpty() || !runtimeDowngradeStore_) {
        return fail(tr(
            "Runtime downgrade preparation could not establish a supported compatibility state"));
    }

    QString identityDetail;
    const auto executableIdentity =
        tryx::RuntimeDowngradeStore::currentExecutableIdentity(
            &identityDetail);
    if (!executableIdentity.isValid()) {
        qWarning().noquote()
            << "Runtime downgrade executable identity failed:"
            << identityDetail;
        return fail(tr(
            "The current runtime executable identity could not be verified"));
    }

    if (!worker_ ||
        (worker_->thread() != QThread::currentThread() &&
         !workerThread_.isRunning())) {
        return fail(tr(
            "Runtime downgrade preparation could not establish a supported compatibility state"));
    }

    // The main-thread latch above prevents new requests. Close the PASE
    // generation gate immediately, then place a blocking fence in the shared
    // worker queue. When it returns, every command accepted before the latch
    // has completed and all legacy/PASE transport timers are stopped. Only
    // this proven quiescent state may be recorded in the durable marker.
    stopKeepalive();
    if (artifactSweepTimer_) {
        artifactSweepTimer_->stop();
    }
    ++printerGeneration_;
    emit requestCancelPrinterPreparation(printerGeneration_);
    worker_->updatePrinterGenerationGate(
        printerGeneration_, false);
    bool workerQuiesced = true;
    if (worker_->thread() == QThread::currentThread()) {
        worker_->quiesceForRuntimeDowngrade(
            printerGeneration_);
    } else {
        workerQuiesced = QMetaObject::invokeMethod(
            worker_,
            [worker = worker_, generation = printerGeneration_]() {
                worker->quiesceForRuntimeDowngrade(generation);
            },
            Qt::BlockingQueuedConnection);
    }
    const auto failAfterWorkerFence =
        [this, &setFailure](const QString &message) {
            runtimeDowngradeV10Mode_.clear();
            setFailure(message);
            emit runtimeDowngradeV10PreparedForExit();
            return false;
        };
    if (!workerQuiesced) {
        return failAfterWorkerFence(tr(
            "The device transport could not be quiesced for runtime downgrade; the runtime is stopping safely"));
    }

    const auto finalSafety =
        retryCacheStore_->releasedV10DowngradeSafety(
            retryCacheSnapshot_);
    if (!finalSafety.safe ||
        finalSafety.status != safety.status) {
        return failAfterWorkerFence(tr(
            "Stored retry media is not exactly compatible with runtime v10"));
    }
    QString finalIdentityDetail;
    const auto finalExecutableIdentity =
        tryx::RuntimeDowngradeStore::currentExecutableIdentity(
            &finalIdentityDetail);
    if (!finalExecutableIdentity.isValid() ||
        finalExecutableIdentity.path != executableIdentity.path ||
        finalExecutableIdentity.device != executableIdentity.device ||
        finalExecutableIdentity.inode != executableIdentity.inode ||
        finalExecutableIdentity.size != executableIdentity.size ||
        finalExecutableIdentity.sha256 != executableIdentity.sha256) {
        qWarning().noquote()
            << "Runtime downgrade executable identity changed after transport quiesce:"
            << finalIdentityDetail;
        return failAfterWorkerFence(tr(
            "The current runtime executable identity could not be verified"));
    }

    const auto persisted = runtimeDowngradeStore_->persist(
        compatibilityMode, retryCacheSnapshot_.storeRevision,
        finalExecutableIdentity);
    if (!persisted.ok) {
        qWarning().noquote()
            << "Runtime downgrade marker commit failed:"
            << persisted.detail;
        if (persisted.commitMayExist) {
            // Once rename may have committed the authoritative marker, the
            // mutation latch is irreversible in this process. The caller
            // still receives a failure, while the runtime exits cleanly and
            // the startup gate resolves the durable outcome on next launch.
            return failAfterWorkerFence(tr(
                "The runtime downgrade marker commit outcome is uncertain; the runtime is stopping safely"));
        }
        return failAfterWorkerFence(tr(
            "The runtime downgrade marker could not be committed durably; the runtime is stopping safely"));
    }

    runtimeDowngradeV10Mode_ = compatibilityMode;
    if (mode) {
        *mode = compatibilityMode;
    }
    qInfo().noquote()
        << "Runtime downgrade v10 prepared with compatibility state"
        << compatibilityMode;
    emit runtimeDowngradeV10PreparedForExit();
    return true;
}

QString DeviceManager::supportSnapshotV1(
    const TryxRuntimeSnapshot &connection) const {
    tryx::SupportSnapshotSourceV1 source;
    source.runtimeVersion = QCoreApplication::applicationVersion();
    if (source.runtimeVersion.isEmpty()) {
        source.runtimeVersion = QStringLiteral("unavailable");
    }
    source.runtimeApiVersion = tryxRuntimeApiVersion();
    source.connection = connection;
    source.physicalGeneration = printerGeneration_;
    source.recoveryRequired = printerRecoveryRequired_;
    source.firmwareRecoveryInterlockActive =
        firmwareRecoveryInterlockActive_;
    source.mediaCatalogEntryCount = mediaCatalog_.entries.size();
    source.artifactCount = deviceMediaArtifactStore_
        ? deviceMediaArtifactStore_->size()
        : 0;
    source.operationCount = operations_.size();
    source.retryCandidatePresent =
        retryCacheSnapshot_.retryCandidate.has_value();
    source.retryDispatchPresent =
        retryCacheSnapshot_.inFlightDispatch.has_value();
    source.retryCleanupPendingCount =
        retryCacheSnapshot_.cleanupPending.size();
    source.deleteRecoveryPresent = pendingDeleteIntent_.has_value();
    source.replaceRecoveryPresent =
        !pendingReplaceJournalOperationId_.isEmpty();

    const qsizetype firstOperation = std::max<qsizetype>(
        0, operationOrder_.size() - 32);
    for (qsizetype index = firstOperation;
         index < operationOrder_.size(); ++index) {
        const auto found = operations_.constFind(
            operationOrder_.at(index));
        if (found == operations_.constEnd()) {
            continue;
        }
        source.operations.append(found->info);
        source.replaceRecoveryPresent =
            source.replaceRecoveryPresent ||
            found->replaceJournalActive;
    }
    return tryx::buildSupportSnapshotV1(source);
}

TryxRuntimeOperationInfo DeviceManager::operationInfo(
    const QString &operationId) const {
    const auto found = operations_.constFind(operationId);
    return found == operations_.constEnd() ? TryxRuntimeOperationInfo{}
                                           : found->info;
}

TryxRuntimeOperationInfo DeviceManager::activeOperationInfo() const {
    return operationInfo(activeOperationId_);
}

QStringList DeviceManager::metricsCapabilities() const {
    return tryxMetricsCatalog();
}

void DeviceManager::publishMetricsState() {
    ++metricsState_.revision;
    emit metricsStateUpdated(metricsState_);
}

void DeviceManager::publishDisplayState() {
    ++displayState_.revision;
    emit displayStateUpdated(displayState_);
}

void DeviceManager::updateDisplayState(
    const PrinterProtocol::PaseDisplayState &state,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    const int previousBrightness = displayState_.brightness;
    const bool hadValidState = displayState_.valid;
    displayState_.deviceSerial = printerDeviceSerial_.trimmed();
    displayState_.valid = true;
    displayState_.backlightEnabled = state.backlightEnabled;
    displayState_.brightness = state.brightness;
    displayState_.standbyEnabled = state.standbyEnabled;
    displayState_.standbyMedia = state.standbyMedia;
    displayState_.mirrorMode = state.mirrorMode;
    displayState_.waterfallMode = state.waterfallMode;
    displayState_.screenMode = state.screenMode;
    displayState_.playMode = state.playMode;
    displayState_.media = state.media;
    displayState_.sysinfoLabels = overlay.left.metrics;
    displayState_.settingsBadges = overlay.left.badges;
    displayState_.settingsPosition =
        overlay.left.verticalPlacement;
    displayState_.settingsColor =
        paseTextColorName(overlay.left.textColor);
    displayState_.settingsAlign = overlay.left.alignment;
    if (overlay.dualMode) {
        displayState_.sysinfoLabels2 = overlay.right.metrics;
        displayState_.settingsBadges2 = overlay.right.badges;
        displayState_.settingsPosition2 =
            overlay.right.verticalPlacement;
        displayState_.settingsColor2 =
            paseTextColorName(overlay.right.textColor);
        displayState_.settingsAlign2 =
            overlay.right.alignment;
    } else {
        displayState_.sysinfoLabels2.clear();
        displayState_.settingsBadges2.clear();
        displayState_.settingsPosition2.clear();
        displayState_.settingsColor2.clear();
        displayState_.settingsAlign2.clear();
    }
    displayState_.diagnostic.clear();
    publishDisplayState();
    if (!hadValidState ||
        previousBrightness != displayState_.brightness) {
        emit brightnessChanged(displayState_.brightness);
    }
}

TryxRuntimeMediaCatalogSnapshot DeviceManager::mediaCatalogSnapshot() const {
    return mediaCatalog_;
}

TryxRuntimeDeviceCapabilitiesV1 DeviceManager::deviceCapabilitiesV1(
    quint64 connectionRevision) const {
    TryxRuntimeDeviceCapabilitiesV1 snapshot;
    snapshot.connectionRevision = connectionRevision;
    snapshot.physicalGeneration = printerGeneration_;
    if (!connected_ || !printerClassConnected_) {
        return snapshot;
    }

    snapshot.deviceIdentity = printerDeviceSerial_.trimmed();
    const std::optional<PrinterProductProfile> profile =
        currentPrinterProductProfile();
    if (snapshot.deviceIdentity.isEmpty() || !profile) {
        snapshot.deviceIdentity.clear();
        return snapshot;
    }

    if (profile->mediaUploadSupported) {
        snapshot.capabilities.append(
            tryxDeviceMediaUploadV1Token());
    }
    if (profile->mediaCatalogSupported) {
        snapshot.capabilities.append(
            tryxDeviceMediaCatalogV1Token());
    }
    if (profile->displayConfigurationSupported) {
        snapshot.capabilities.append(
            tryxDeviceDisplayConfigurationV1Token());
    }
    if (profile->splitAreaMediaSupported) {
        snapshot.capabilities.append(
            tryxDeviceMediaSplitAreaV1Token());
    }
    if (profile->overlayMetricsSupported) {
        snapshot.capabilities.append(
            tryxDeviceOverlayMetricsV1Token());
    }
    if (profile->firmwareFlashSupported) {
        snapshot.capabilities.append(
            tryxDeviceFirmwareFlashV1Token());
    }
    return snapshot;
}

TryxRuntimeDeviceSpecificationsV1 DeviceManager::deviceSpecificationsV1(
    const TryxRuntimeSnapshot &connection) const {
    TryxRuntimeDeviceSpecificationsV1 snapshot;
    snapshot.connectionRevision = connection.revision;
    if (!connection.connected) {
        return snapshot;
    }

    snapshot.deviceIdentity = connection.serial.trimmed();
    if (!connection.printerClassConnected) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }

    const QString currentIdentity = printerDeviceSerial_.trimmed();
    const bool exactConnectionContext =
        connected_ && printerClassConnected_ &&
        printerGeneration_ != 0 &&
        !currentIdentity.isEmpty() &&
        snapshot.deviceIdentity == currentIdentity &&
        connection.productId == printerProductIdString(printerProductId_);
    if (!exactConnectionContext) {
        snapshot.deviceIdentity.clear();
        snapshot.physicalGeneration = 0;
        return snapshot;
    }

    snapshot.physicalGeneration = printerGeneration_;
    if (printerProductId_ != 0x1011 && printerProductId_ != 0x1021) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }

    snapshot.status = QStringLiteral("Unavailable");
    const bool exactCache =
        deviceSpecificationsCache_.valid &&
        deviceSpecificationsDevicePath_ == printerDevicePath_ &&
        deviceSpecificationsDeviceIdentity_ == currentIdentity &&
        deviceSpecificationsProductId_ == printerProductId_ &&
        deviceSpecificationsGeneration_ == printerGeneration_;
    if (!exactCache) {
        return snapshot;
    }

    snapshot.status = QStringLiteral("Ready");
    snapshot.reportedProductName =
        deviceSpecificationsCache_.reportedProductName;
    snapshot.videoOutputWidth =
        deviceSpecificationsCache_.videoOutputWidth;
    snapshot.videoOutputHeight =
        deviceSpecificationsCache_.videoOutputHeight;
    snapshot.screenType = deviceSpecificationsCache_.screenType;
    snapshot.usbAutoKeepalive =
        deviceSpecificationsCache_.usbAutoKeepalive;
    return snapshot;
}

QString DeviceManager::mediaCatalogDirectory() const {
    return mediaCatalogStore_->rootDirectory();
}

QString DeviceManager::mediaThumbnailPath(
    const QString &thumbnailKey) const {
    return mediaCatalogStore_->thumbnailPath(thumbnailKey);
}

void DeviceManager::loadMediaCatalogStore() {
    const auto result = mediaCatalogStore_->load();
    for (const QString &warning : result.warnings) {
        qWarning().noquote() << warning;
    }
}

void DeviceManager::loadSavedLayoutsStore() {
    savedLayoutsStoreLoaded_ = true;
    savedLayoutsFailureDetail_.clear();
    const tryx::SavedLayoutStore::LoadResult result =
        savedLayoutStore_->load();
    if (!result.writesEnabled) {
        savedLayoutsFailureDetail_ = result.detail.isEmpty()
            ? tr("Saved layouts could not be loaded safely")
            : result.detail.left(512);
        qWarning().noquote() << savedLayoutsFailureDetail_;
    }
}

bool DeviceManager::currentSavedLayoutsContext(
    QString *deviceIdentity, QString *productId) const {
    if (deviceIdentity) {
        deviceIdentity->clear();
    }
    if (productId) {
        productId->clear();
    }
    if (!connected_ || !printerClassConnected_ ||
        printerGeneration_ == 0 || currentPrinterPath().isEmpty()) {
        return false;
    }
    const QString identity = printerDeviceSerial_;
    const QString product = printerProductIdString(printerProductId_);
    const auto profile = currentPrinterProductProfile();
    const bool valid =
        tryxSavedLayoutDeviceIdentityIsCanonical(identity) &&
        tryxSavedLayoutProductIdIsSupported(product) && profile &&
        (profile->productId == 0x1011 ||
         profile->productId == 0x1021) &&
        profile->mediaCatalogSupported &&
        profile->displayConfigurationSupported &&
        profile->overlayMetricsSupported;
    if (!valid) {
        return false;
    }
    if (deviceIdentity) {
        *deviceIdentity = identity;
    }
    if (productId) {
        *productId = product;
    }
    return true;
}

bool DeviceManager::buildSavedLayoutMediaProof(
    const TryxRuntimeApplyRequest &request,
    QList<TryxRuntimeSavedMediaRefV1> *proof,
    QString *errorMessage) const {
    if (proof) {
        proof->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    QString deviceIdentity;
    QString productId;
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    if (!proof ||
        !currentSavedLayoutsContext(&deviceIdentity, &productId) ||
        mediaCatalog_.deviceIdentity != deviceIdentity) {
        return fail(tr(
            "The authoritative media catalog is unavailable for this device"));
    }
    const bool split = request.screenMode ==
        QStringLiteral("Screen Splitting");
    const auto profile = currentPrinterProductProfile();
    if ((split && (!profile || !profile->splitAreaMediaSupported)) ||
        request.media.size() != (split ? 2 : 1)) {
        return fail(tr(
            "The saved layout media count is not supported by this device"));
    }

    QSet<QString> seenNames;
    for (const QString &name : request.media) {
        if (name.isEmpty() || seenNames.contains(name)) {
            return fail(tr(
                "The saved layout media selection is ambiguous"));
        }
        seenNames.insert(name);
        QList<TryxRuntimeMediaEntry> matches;
        for (const TryxRuntimeMediaEntry &entry : mediaCatalog_.entries) {
            if (entry.name == name) {
                matches.append(entry);
            }
        }
        if (matches.size() != 1) {
            return fail(tr(
                "The saved layout media is missing or ambiguous in the current catalog"));
        }
        const TryxRuntimeMediaEntry &entry = matches.constFirst();
        if (!isSha256Hex(entry.mediaId) || entry.size == 0 ||
            (entry.source != 1U && entry.source != 2U)) {
            return fail(tr(
                "The saved layout media identity is incomplete"));
        }
        TryxRuntimeSavedMediaRefV1 reference;
        reference.mediaId = entry.mediaId;
        reference.name = entry.name;
        reference.size = entry.size;
        reference.source = entry.source;
        reference.readOnly = entry.readOnly;
        proof->append(reference);
    }
    return true;
}

void DeviceManager::updateMediaCatalog(
    const QList<PrinterProtocol::MediaFile> &mediaFiles) {
    if (cacheCleanupExclusiveActive_) {
        deferredMediaCatalogFiles_ = mediaFiles;
        deferredMediaCatalogGeneration_ = printerGeneration_;
        deferredMediaCatalogDeviceIdentity_ =
            printerDeviceSerial_.trimmed();
        deferredMediaCatalogUpdatePending_ = true;
        return;
    }
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    snapshot.deviceIdentity = printerDeviceSerial_.trimmed();
    QList<tryx::MediaCatalogStore::RemoteEntry> freshEntries;
    freshEntries.reserve(mediaFiles.size());
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        TryxRuntimeMediaEntry entry;
        entry.name = media.name;
        entry.size = media.size;
        entry.source = media.source == PrinterProtocol::MediaSource::Preset
            ? 2U
            : 1U;
        entry.readOnly = media.readOnly;
        const auto remote = mediaCatalogRemoteEntry(entry);
        freshEntries.append(remote);
        const auto decoration = mediaCatalogStore_->decoration(
            snapshot.deviceIdentity, remote);
        entry.mediaId = decoration.mediaId;
        entry.thumbnailKey = decoration.thumbnailKey;
        entry.managedOrigin = decoration.managedOrigin;
        if (entry.source == 2U) {
            entry.deleteBlockReason = QStringLiteral("Preset");
        } else if (entry.readOnly) {
            entry.deleteBlockReason = QStringLiteral("ReadOnly");
        } else if (entry.name.startsWith(
                       QStringLiteral("default_"),
                       Qt::CaseInsensitive)) {
            entry.deleteBlockReason = QStringLiteral("ProtectedName");
        } else {
            entry.deleteAllowed = true;
        }
        snapshot.entries.append(entry);
    }
    const auto prune = mediaCatalogStore_->pruneAuthoritative(
        snapshot.deviceIdentity, freshEntries);
    if (!prune.ok()) {
        qWarning().noquote()
            << QStringLiteral("Cannot prune media catalog index: %1")
                   .arg(prune.detail);
    }
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

void DeviceManager::clearMediaCatalogView() {
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

void DeviceManager::loadRuntimePresentationPreferences() {
    presentationPreferences_ = {};
    if (!runtimePresentationPreferencesStore_) {
        qWarning().noquote()
            << "The runtime presentation preferences store is unavailable;"
            << "using Celsius and 24H";
    } else {
        const auto result = runtimePresentationPreferencesStore_->load();
        presentationPreferences_ = result.preferences;
        presentationPreferences_.revision = 1;
        if (!result.detail.isEmpty()) {
            qWarning().noquote() << result.detail;
        }
    }
    const TryxRuntimePresentationPreferencesV1 loaded =
        presentationPreferences_;
    worker_->publishPresentationPreferences(loaded);
    if (worker_->thread() == QThread::currentThread()) {
        worker_->applyPublishedPresentationPreferences();
    } else {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() {
                worker->applyPublishedPresentationPreferences();
            },
            Qt::QueuedConnection);
    }
}

void DeviceManager::loadPaseMetricsConfig() {
    const auto result = paseMetricsConfigStore_->load();
    for (const QString &warning : result.warnings) {
        qWarning().noquote() << warning;
    }
}

bool DeviceManager::persistPaseMetricsConfiguration(
    const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
    QString *errorMessage) {
    const QString serial = printerDeviceSerial_.trimmed();
    if (serial.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Cannot persist PASE metrics without a device serial");
        }
        return false;
    }
    const auto result = enabled
        ? paseMetricsConfigStore_->persist(serial, overlay)
        : paseMetricsConfigStore_->clearForDevice(serial);
    if (!result.ok() && errorMessage) {
        *errorMessage = result.detail;
    }
    return result.ok();
}

PrinterProtocol::PaseOverlayConfig
DeviceManager::persistedPaseOverlayForDevice(
    const QString &deviceSerial) const {
    const auto overlay =
        paseMetricsConfigStore_->overlayForDevice(deviceSerial);
    return overlay.value_or(PrinterProtocol::PaseOverlayConfig{});
}

QString DeviceManager::promoteThumbnailForOperation(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry) {
    if (cacheCleanupExclusiveActive_) {
        return {};
    }
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() ||
        found->stagedThumbnailPath.isEmpty() ||
        !isSha256Hex(found->stagedThumbnailSha256) ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        return {};
    }

    tryx::MediaCatalogStore::ThumbnailInput input;
    input.deviceIdentity = printerDeviceSerial_.trimmed();
    input.remote = mediaCatalogRemoteEntry(verifiedEntry);
    input.stagedPath = found->stagedThumbnailPath;
    input.stagedSha256 = found->stagedThumbnailSha256;
    const auto committed = mediaCatalogStore_->commitThumbnail(input);
    if (!committed.result.ok()) {
        if (committed.result.code !=
                tryx::MediaCatalogStore::ErrorCode::InvalidInput &&
            committed.result.code !=
                tryx::MediaCatalogStore::ErrorCode::UnsafeSource) {
            qWarning().noquote()
                << QStringLiteral("Cannot commit media thumbnail index: %1")
                       .arg(committed.result.detail);
        }
        return {};
    }
    return committed.thumbnailKey;
}

bool DeviceManager::commitVerifiedMediaMetadata(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *errorCategory, QString *errorMessage) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd()) {
        if (errorCategory) {
            *errorCategory = QStringLiteral(
                "LocalMediaCommitFailed");
        }
        if (errorMessage) {
            *errorMessage = tr(
                "The verified upload operation is no longer available for local catalog commit");
        }
        return false;
    }

    if (!found->stagedThumbnailPath.isEmpty() &&
        promoteThumbnailForOperation(
            operationId, verifiedEntry).isEmpty()) {
        if (errorCategory) {
            *errorCategory = QStringLiteral(
                "ThumbnailPersistenceFailed");
        }
        if (errorMessage) {
            *errorMessage = tr(
                "The upload is present on the device, but its preview could not be persisted locally");
        }
        return false;
    }

    const bool originRequired = found->ensureExisting ||
        isSha256Hex(found->sourceContentSha256);
    if (originRequired) {
        QString originError;
        if (!persistMediaOriginForOperation(
                operationId, verifiedEntry, &originError)) {
            if (errorCategory) {
                *errorCategory = QStringLiteral(
                    "OriginPersistenceFailed");
            }
            if (errorMessage) {
                *errorMessage = tr(
                    "The upload is present on the device, but its content identity could not be persisted: %1")
                                    .arg(originError);
            }
            return false;
        }
    }
    return true;
}

bool DeviceManager::persistMediaOriginForOperation(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *errorMessage) {
    if (cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = tr(
                "Media catalog updates are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto found = operations_.constFind(operationId);
    const QString deviceIdentity = printerDeviceSerial_.trimmed();
    if (found == operations_.constEnd() || deviceIdentity.isEmpty() ||
        !isSha256Hex(found->sourceContentSha256) ||
        !isSha256Hex(found->preparedSha256) || found->sourceSize <= 0 ||
        found->conversionProfile.isEmpty() ||
        verifiedEntry.source != 1U || verifiedEntry.readOnly ||
        verifiedEntry.name != found->remoteName) {
        if (errorMessage) {
            *errorMessage = tr(
                "Confirmed media does not have a complete origin identity");
        }
        return false;
    }

    tryx::MediaCatalogStore::OriginInput input;
    input.deviceIdentity = deviceIdentity;
    input.remote = mediaCatalogRemoteEntry(verifiedEntry);
    input.sourceContentSha256 = found->sourceContentSha256;
    input.sourceSize = found->sourceSize;
    input.conversionProfile = found->conversionProfile;
    input.preparedSha256 = found->preparedSha256;
    input.operationId = operationId;
    input.confirmedUtc = QDateTime::currentDateTimeUtc();
    const auto persisted = mediaCatalogStore_->persistOrigin(input);
    if (!persisted.ok()) {
        if (errorMessage) {
            *errorMessage = persisted.detail;
        }
        return false;
    }
    return true;
}

QString DeviceManager::findReusableMediaOrigin(
    const QString &sourceContentSha256,
    const QString &conversionProfile,
    const QList<PrinterProtocol::MediaFile> &mediaFiles) const {
    QList<tryx::MediaCatalogStore::RemoteEntry> freshEntries;
    freshEntries.reserve(mediaFiles.size());
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        freshEntries.append(mediaCatalogRemoteEntry(media));
    }
    return mediaCatalogStore_->findReusableOrigin(
        printerDeviceSerial_.trimmed(), sourceContentSha256,
        conversionProfile, freshEntries);
}

void DeviceManager::publishOperation(const QString &operationId) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd()) {
        return;
    }
    const quint64 revision = ++operationRevision_;
    qInfo().noquote()
        << QStringLiteral("operation=%1 generation=%2 state=%3 stage=%4 completed=%5 total=%6")
               .arg(found->info.id)
               .arg(found->info.deviceGeneration)
               .arg(found->info.state, found->info.stage)
               .arg(found->info.completed)
               .arg(found->info.total);
    emit operationChanged(found->info, revision);
}

void DeviceManager::finishOperation(const QString &operationId,
                                    const QString &state,
                                    const QString &errorCategory,
                                    const QString &retryMode,
                                    const QString &message,
                                    bool preserveReplaceJournal) {
    if (!operationIsTerminal(state)) {
        qWarning().noquote()
            << QStringLiteral(
                   "Refusing to finish operation %1 with non-terminal state %2")
                   .arg(operationId, state);
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end() || operationIsTerminal(found->info.state)) {
        return;
    }
    QString terminalMessage = message;
    if (!preserveReplaceJournal && found->replaceOperation &&
        found->replaceJournalActive) {
        const bool replacementMutationMayHaveStarted =
            found->replaceJournal.applyMayHaveStarted ||
            found->replaceJournal.fileRemoveMayHaveStarted;
        const bool reconciliationRequired =
            retryMode == QStringLiteral("DeleteReconcile") ||
            (replacementMutationMayHaveStarted &&
             (retryMode == QStringLiteral("ReconcileOnly") ||
              errorCategory == QStringLiteral("PartialOrUnknown")));
        QString journalError;
        if (reconciliationRequired) {
            found->replaceJournal.disposition =
                QStringLiteral("PartialOrUnknown");
            const QString journalStage =
                found->replaceJournal.fileRemoveMayHaveStarted
                    ? QStringLiteral("DeleteReconciliation")
                    : found->replaceJournal.applyMayHaveStarted
                        ? QStringLiteral("ApplyVerification")
                        : found->replaceJournal.uploadVerified
                            ? QStringLiteral("UploadVerified")
                            : found->replaceJournal.stage;
            if (!writeReplaceJournal(
                    operationId, journalStage,
                    &journalError)) {
                terminalMessage += tr(
                    " Replace reconciliation state could not be persisted: %1")
                                       .arg(journalError);
            }
        } else {
            found->replaceJournal.disposition =
                found->info.terminalOutcome ==
                        QStringLiteral("Replaced")
                    ? QStringLiteral("Replaced")
                    : found->replaceJournal.uploadVerified
                        ? QStringLiteral("NewCopyReady")
                        : QStringLiteral("OriginalRetained");
            if (!writeReplaceJournal(
                    operationId, QStringLiteral("Terminal"),
                    &journalError)) {
                terminalMessage += tr(
                    " Terminal replace state could not be persisted: %1")
                                       .arg(journalError);
            } else if (!clearReplaceJournal(&journalError)) {
                terminalMessage += tr(
                    " Terminal replace journal could not be removed: %1")
                                       .arg(journalError);
            }
        }
    }
    found->info.state = state;
    found->info.stage = state;
    found->info.errorCategory = errorCategory;
    found->info.retryMode = retryMode;
    found->info.message = terminalMessage;
    emit requestEndPrinterForegroundOperation(
        operationId, found->info.deviceGeneration);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    if (worker_) {
        worker_->clearPrinterOperationCancellation(operationId);
    }
    releaseArtifactOperationHold(operationId);
    releaseOwnedSource(*found);
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::pauseOperationForRetryCacheReconciliation(
    const QString &operationId, const QString &errorCategory,
    const QString &message) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    found->info.state = QStringLiteral("Refreshing");
    found->info.stage = QStringLiteral("RecoveringFinalization");
    found->info.errorCategory = errorCategory;
    found->info.retryMode.clear();
    found->info.message = message;
    emit requestEndPrinterForegroundOperation(
        operationId, found->info.deviceGeneration);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    if (worker_) {
        worker_->clearPrinterOperationCancellation(operationId);
    }
    publishOperation(operationId);
}

void DeviceManager::rejectOperation(const QString &operationId,
                                    const QString &kind,
                                    const QString &subject,
                                    const QString &category,
                                    const QString &message) {
    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Failed");
    record.info.stage = QStringLiteral("Rejected");
    record.info.errorCategory = category;
    record.info.subject = subject;
    record.info.message = message;
    record.info.deviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::rejectSavedLayoutApplyOperation(
    const QString &operationId, const QString &subject,
    const QString &category, const QString &message) {
    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("SavedLayoutApply");
    record.info.state = QStringLiteral("Failed");
    record.info.stage = QStringLiteral("Rejected");
    record.info.errorCategory = category;
    record.info.terminalOutcome = QStringLiteral("NotStarted");
    record.info.subject = subject;
    record.info.message = message;
    record.info.deviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::pruneOperationHistory() {
    int terminalCount = 0;
    for (const QString &operationId : std::as_const(operationOrder_)) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd() &&
            operationIsTerminal(found->info.state)) {
            ++terminalCount;
        }
    }
    while (terminalCount > kMaxTerminalOperationHistory) {
        bool removed = false;
        for (qsizetype index = 0; index < operationOrder_.size(); ++index) {
            const QString operationId = operationOrder_.at(index);
            const auto found = operations_.constFind(operationId);
            if (found == operations_.constEnd() ||
                !operationIsTerminal(found->info.state) ||
                operationId == retryCacheVisibleOperationId() ||
                (!found->artifactId.isEmpty() &&
                 deviceMediaArtifactStore_->contains(
                     found->artifactId))) {
                continue;
            }
            operations_.remove(operationId);
            operationOrder_.removeAt(index);
            --terminalCount;
            const quint64 revision = ++operationRevision_;
            emit operationRemoved(operationId, revision);
            removed = true;
            break;
        }
        if (!removed) {
            break;
        }
    }
}

QString DeviceManager::queueStageDeviceMediaOperation(
    const QString &requestedOperationId, const QString &mediaId,
    const QString &ownerUniqueName) {
    if (!tryx::DeviceMediaArtifactStore::isValidDbusUniqueName(
            ownerUniqueName)) {
        return {};
    }
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = QStringLiteral("StageDeviceMedia");
    if (operations_.contains(operationId)) {
        const OperationRecord &existing =
            operations_.value(operationId);
        const auto existingArtifact =
            deviceMediaArtifactStore_->artifact(existing.artifactId);
        return existing.info.kind == kind &&
                       existing.artifactOwner == ownerUniqueName &&
                       existing.originalMediaId == mediaId &&
                       (existing.info.state ==
                                QStringLiteral("Failed") ||
                        (existingArtifact.ok() &&
                         existingArtifact.artifact.metadata.mediaId ==
                             mediaId))
            ? operationId
            : QString();
    }
    const auto reject =
        [this, &operationId, &kind, &mediaId,
         &ownerUniqueName](
                            const QString &category,
                            const QString &message) {
        rejectOperation(operationId, kind, mediaId,
                        category, message);
        OperationRecord &record =
            operations_[operationId];
        record.originalMediaId = mediaId;
        record.artifactOwner = ownerUniqueName;
        return operationId;
    };
    if (firmwareExclusiveActive()) {
        return reject(
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
    }
    if (!isSha256Hex(mediaId)) {
        return reject(
            QStringLiteral("InvalidMediaId"),
            tr("The selected device media identity is invalid"));
    }
    if (retryCacheMutationGateActive()) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
    }
    if (!currentPrinterSupportsMediaCatalog()) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tr("Media catalog export is not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (printerRecoveryRequired_ ||
        printerDisplaySessionLost_) {
        return reject(
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerUnavailableStatusText());
    }
    if (!printerDisplaySessionActive_) {
        return reject(
            QStringLiteral("SessionNotReady"),
            printerUnavailableStatusText());
    }
    const QString devicePath = currentPrinterPath();
    const QString deviceIdentity =
        printerDeviceSerial_.trimmed();
    if (devicePath.isEmpty() ||
        deviceIdentity.isEmpty() ||
        mediaCatalog_.deviceIdentity != deviceIdentity) {
        return reject(
            QStringLiteral("DeviceIdentityUnavailable"),
            tr("The current PASE media catalog is not associated with the active device"));
    }
    const TryxRuntimeMediaEntry *entry =
        findMediaById(mediaId);
    if (!entry ||
        mediaCatalogStore_->mediaId(
            deviceIdentity, mediaCatalogRemoteEntry(*entry)) != mediaId ||
        entry->source != 1U || entry->readOnly ||
        entry->size == 0 ||
        entry->size >
            static_cast<quint64>(kMaxRetryCacheBytes) ||
        !PrinterProtocol::isSafeUploadMediaName(entry->name)) {
        return reject(
            QStringLiteral("DeviceMediaNotEligible"),
            tr("Only an exact writable user media entry can be exported or edited"));
    }
    tryx::DeviceMediaArtifactStore::ReservationInput reservation;
    reservation.operationId = operationId;
    reservation.mediaId = mediaId;
    reservation.deviceIdentity = deviceIdentity;
    reservation.remoteName = entry->name;
    reservation.expectedSize = entry->size;
    reservation.logicalType = QStringLiteral("Video");
    reservation.ownerUniqueName = ownerUniqueName;
    const auto reserved =
        deviceMediaArtifactStore_->reserve(reservation);
    if (!reserved.ok()) {
        if (reserved.result.code ==
                tryx::DeviceMediaArtifactStore::ErrorCode::OutboxUnavailable ||
            reserved.result.code ==
                tryx::DeviceMediaArtifactStore::ErrorCode::CleanupIncomplete) {
            return reject(
                QStringLiteral("OutboxUnavailable"),
                tr("The private device media outbox is unavailable: %1")
                    .arg(reserved.result.detail));
        }
        return reject(
            QStringLiteral("ArtifactAllocationFailed"),
            tr("Could not allocate a unique device media artifact"));
    }
    const QString artifactId =
        reserved.artifact.metadata.artifactId;

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Pulling");
    record.info.stage =
        QStringLiteral("FreshCatalogPreflight");
    record.info.subject = entry->name;
    record.info.message =
        tr("Validating the current device media entry...");
    record.info.total =
        static_cast<qint64>(entry->size);
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = printerProductId_;
    record.artifactId = artifactId;
    record.artifactOwner = ownerUniqueName;
    record.originalMediaId = mediaId;
    record.uploadDeviceIdentity = deviceIdentity;
    record.uploadDeviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (!watchArtifactOwner(ownerUniqueName)) {
        deviceMediaArtifactStore_->ownerDisconnected(
            ownerUniqueName);
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"), QString(),
            tr("The requesting D-Bus client disconnected before device media staging began"));
        return operationId;
    }
    emit requestBeginPrinterForegroundOperation(
        operationId, printerGeneration_);
    emit requestPrinterStageMedia(
        devicePath, entry->name,
        static_cast<qint64>(entry->size),
        reserved.artifact.canonicalPath, operationId,
        printerGeneration_);
    return operationId;
}

TryxRuntimeDeviceMediaArtifact
DeviceManager::claimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (runtimeDowngradeV10Prepared_ || cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = runtimeDowngradeV10Prepared_
                ? tr("Device mutations are blocked because runtime downgrade preparation is committed")
                : tr("Device media leases are blocked while temporary files are being cleaned");
        }
        return {};
    }
    const auto operation = operations_.constFind(operationId);
    const auto artifact =
        deviceMediaArtifactStore_->artifact(artifactId);
    if (operation == operations_.constEnd() ||
        operation->info.kind !=
            QStringLiteral("StageDeviceMedia") ||
        operation->info.state != QStringLiteral("Succeeded") ||
        operation->info.resultName != artifactId ||
        !artifact.ok() ||
        artifact.artifact.metadata.operationId != operationId) {
        if (errorMessage) {
            *errorMessage = tr(
                "The requested stage operation has no unclaimed artifact");
        }
        return {};
    }
    const auto claimed = deviceMediaArtifactStore_->claim(
        artifactId, operationId, ownerUniqueName);
    if (!claimed.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                claimed.result.code, claimed.result.detail);
        }
        return {};
    }
    return runtimeDeviceMediaArtifact(claimed);
}

TryxRuntimeDeviceMediaMetadataV1
DeviceManager::deviceMediaMetadataV1(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) const {
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "The device media artifact metadata requires an active lease");
        }
        return {};
    }
    const auto inspected = deviceMediaArtifactStore_->inspectClaimed(
        artifactId, leaseId, ownerUniqueName, false);
    if (!inspected.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                inspected.result.code, inspected.result.detail);
        }
        return {};
    }
    const TryxRuntimeDeviceMediaMetadataV1 metadata =
        runtimeDeviceMediaMetadata(inspected.artifact.metadata);
    if (!tryxRuntimeDeviceMediaMetadataV1IsValid(metadata)) {
        if (errorMessage) {
            *errorMessage = tr(
                "The device media artifact metadata is unavailable");
        }
        return {};
    }
    return metadata;
}

bool DeviceManager::renewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (runtimeDowngradeV10Prepared_ || cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = runtimeDowngradeV10Prepared_
                ? tr("Device mutations are blocked because runtime downgrade preparation is committed")
                : tr("Device media leases are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto artifact =
        deviceMediaArtifactStore_->artifact(artifactId);
    if (!artifact.ok() || !artifact.artifact.claimed ||
        leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "The device media artifact has not been claimed");
        }
        return false;
    }
    const auto renewed = deviceMediaArtifactStore_->renew(
        artifactId, leaseId, ownerUniqueName);
    if (!renewed.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                renewed.code, renewed.detail);
        }
        return false;
    }
    return true;
}

bool DeviceManager::releaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (runtimeDowngradeV10Prepared_ || cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = runtimeDowngradeV10Prepared_
                ? tr("Device mutations are blocked because runtime downgrade preparation is committed")
                : tr("Device media leases are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto artifact =
        deviceMediaArtifactStore_->artifact(artifactId);
    if (!artifact.ok() || !artifact.artifact.claimed ||
        leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "The device media artifact has not been claimed");
        }
        return false;
    }
    const auto released = deviceMediaArtifactStore_->release(
        artifactId, leaseId, ownerUniqueName);
    if (!released.ok() || !released.removed) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                released.code, released.detail);
        }
        return false;
    }
    if (artifactOwnerWatcher_ &&
        !released.ownerUniqueName.isEmpty() &&
        !released.ownerStillUsed) {
        artifactOwnerWatcher_->removeWatchedService(
            released.ownerUniqueName);
    }
    pruneOperationHistory();
    return true;
}

QString DeviceManager::queueRecoveredMediaUploadOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaTransform &transform) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName,
        tryxFullFrameMediaPreparationProfile(transform),
        false, QString(),
        TryxRuntimeApplyRequest{});
}

QString DeviceManager::
queueRecoveredMediaUploadWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, false, QString(),
        TryxRuntimeApplyRequest{});
}

QString DeviceManager::queueReplaceDeviceMediaOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform,
    const QString &ownerUniqueName) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName,
        tryxFullFrameMediaPreparationProfile(transform), true,
        originalMediaId, request);
}

QString DeviceManager::
queueReplaceDeviceMediaWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    const QString &ownerUniqueName) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, true,
        originalMediaId, request);
}

QString DeviceManager::queueRecoveredOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaPreparationProfileV1 &profile, bool replace,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &applyRequest) {
    if (!tryx::DeviceMediaArtifactStore::isValidDbusUniqueName(
            ownerUniqueName)) {
        return {};
    }
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = replace
        ? QStringLiteral("ReplaceDeviceMedia")
        : QStringLiteral("RecoveredMediaUpload");
    QString transformError;
    const TryxRuntimeMediaTransform &transform = profile.transform;
    const QString transformFingerprint =
        tryxMediaTransformFingerprint(transform);
    const QString preparationProfileFingerprint =
        tryxMediaPreparationProfileFingerprint(profile);
    const bool transformValid =
        tryxMediaPreparationProfileV1IsValid(
            profile, &transformError) &&
        !transformFingerprint.isEmpty() &&
        !preparationProfileFingerprint.isEmpty();
    const QString requestedApplyFingerprint =
        runtimeApplyRequestFingerprint(applyRequest);
    if (operations_.contains(operationId)) {
        const OperationRecord &existing =
            operations_.value(operationId);
        const bool sameReplaceRequest =
            !replace ||
            (existing.originalMediaId == originalMediaId &&
             existing.requestedApplyFingerprint ==
                 requestedApplyFingerprint);
        return existing.info.kind == kind &&
                       existing.artifactId == artifactId &&
                       existing.artifactOwner ==
                           ownerUniqueName &&
                       existing.artifactLeaseId == leaseId &&
                       tryxMediaPreparationProfileFingerprint(
                           existing.mediaPreparationProfile) ==
                           preparationProfileFingerprint &&
                       sameReplaceRequest
            ? operationId
            : QString();
    }
    if (cacheCleanupExclusiveActive_) {
        return {};
    }
    const auto heldArtifact =
        deviceMediaArtifactStore_->acquireOperationHold(
            artifactId, operationId, leaseId,
            ownerUniqueName);
    if (!heldArtifact.ok() || leaseId.isEmpty()) {
        return {};
    }
    auto artifact = heldArtifact.artifact;
    if (!watchArtifactOwner(ownerUniqueName)) {
        handleArtifactOwnerUnregistered(ownerUniqueName);
        const auto released =
            deviceMediaArtifactStore_->releaseOperationHold(
                artifactId, operationId);
        if (released.removed && artifactOwnerWatcher_ &&
            !released.ownerUniqueName.isEmpty() &&
            !released.ownerStillUsed) {
            artifactOwnerWatcher_->removeWatchedService(
                released.ownerUniqueName);
        }
        return {};
    }
    const auto reject =
        [this, &operationId, &kind, &artifact,
         &artifactId, &leaseId, &ownerUniqueName,
         &profile, &transform, &originalMediaId,
         &requestedApplyFingerprint](
            const QString &category,
            const QString &message) {
            const auto released =
                deviceMediaArtifactStore_->releaseOperationHold(
                    artifactId, operationId);
            if (released.removed && artifactOwnerWatcher_ &&
                !released.ownerUniqueName.isEmpty() &&
                !released.ownerStillUsed) {
                artifactOwnerWatcher_->removeWatchedService(
                    released.ownerUniqueName);
            }
            rejectOperation(
                operationId, kind,
                artifact.metadata.remoteName,
                category, message);
            OperationRecord &record =
                operations_[operationId];
            record.artifactId = artifactId;
            record.artifactOwner = ownerUniqueName;
            record.artifactLeaseId = leaseId;
            record.mediaTransform = transform;
            record.mediaPreparationProfile = profile;
            record.originalMediaId = originalMediaId;
            record.requestedApplyFingerprint =
                requestedApplyFingerprint;
            return operationId;
        };
    if (firmwareExclusiveActive()) {
        return reject(
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
    }
    if (!transformValid) {
        const bool nestedTransformValid =
            tryxMediaTransformIsValid(transform);
        return reject(
            nestedTransformValid
                ? QStringLiteral("InvalidMediaPreparationProfile")
                : QStringLiteral("InvalidMediaTransform"),
            nestedTransformValid
                ? tr("Media preparation profile is invalid: %1")
                      .arg(transformError)
                : tr("Media transform is invalid: %1")
                      .arg(transformError));
    }
    if (retryCacheMutationGateActive()) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
    }
    if (!currentPrinterSupportsMediaCatalog() ||
        !currentPrinterSupportsDisplayConfiguration()) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tr("Recovered media and replacement are not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
    }
    const std::optional<PrinterProductProfile> productProfile =
        currentPrinterProductProfile();
    if (!productProfile ||
        (profile.target == QStringLiteral("SplitArea") &&
         !productProfile->splitAreaMediaSupported)) {
        return reject(
            QStringLiteral("UnsupportedMediaPreparationTarget"),
            tr("Split-area media preparation is not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (replace &&
        (!pendingDeleteOperationId_.isEmpty() ||
         QFileInfo::exists(deleteIntentPath()) ||
         !pendingReplaceJournalOperationId_.isEmpty() ||
         QFileInfo::exists(replaceIntentPath()))) {
        return reject(
            QStringLiteral("ReplaceReconciliationPending"),
            tr("A previous replacement still requires read-only reconciliation"));
    }
    if (printerRecoveryRequired_ ||
        printerDisplaySessionLost_ ||
        !printerDisplaySessionActive_) {
        return reject(
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty() ||
        artifact.metadata.deviceIdentity !=
            printerDeviceSerial_.trimmed()) {
        return reject(
            QStringLiteral("DeviceChanged"),
            tr("The recovered copy belongs to a different PASE device"));
    }

    TryxRuntimeApplyRequest effectiveApplyRequest =
        applyRequest;
    if (replace &&
        !normalizeAndValidatePaseApplyOverlayStyles(
            &effectiveApplyRequest)) {
        return reject(
            QStringLiteral("UnsupportedReplaceConfiguration"),
            tr("Replace requires a valid overlay style"));
    }
    const TryxRuntimeMediaEntry *originalEntry = nullptr;
    if (replace) {
        originalEntry = findMediaById(originalMediaId);
        if (!originalEntry ||
            originalMediaId !=
                artifact.metadata.mediaId ||
            originalEntry->name !=
                artifact.metadata.remoteName ||
            originalEntry->size !=
                artifact.metadata.size ||
            originalEntry->source != 1U ||
            originalEntry->readOnly ||
            mediaCatalogStore_->mediaId(
                printerDeviceSerial_.trimmed(),
                mediaCatalogRemoteEntry(*originalEntry)) !=
                originalMediaId) {
            return reject(
                QStringLiteral("OriginalMediaChanged"),
                tr("The original media identity changed after the device copy was staged"));
        }
        const bool fullScreen =
            effectiveApplyRequest.screenMode ==
            QStringLiteral("Full Screen");
        const bool splitScreen =
            effectiveApplyRequest.screenMode ==
            QStringLiteral("Screen Splitting");
        const bool targetMatchesLayout =
            (profile.target == QStringLiteral("FullFrame") &&
             fullScreen) ||
            (profile.target == QStringLiteral("SplitArea") &&
             splitScreen);
        const bool currentLayoutMatchesRequest =
            displayState_.valid &&
            displayStateReadGeneration_ == printerGeneration_ &&
            displayState_.screenMode ==
                effectiveApplyRequest.screenMode &&
            displayState_.media == effectiveApplyRequest.media;
        const int expectedCount = splitScreen ? 2 : 1;
        const int originalCount =
            effectiveApplyRequest.media.count(
                originalEntry->name);
        const bool mediaNamesSafe = std::all_of(
            effectiveApplyRequest.media.cbegin(),
            effectiveApplyRequest.media.cend(),
            [](const QString &name) {
                return PrinterProtocol::
                    isSafeUploadMediaName(name);
            });
        if (!targetMatchesLayout ||
            !currentLayoutMatchesRequest ||
            (!fullScreen && !splitScreen) ||
            effectiveApplyRequest.media.size() !=
                expectedCount ||
            originalCount <= 0 || !mediaNamesSafe ||
            (splitScreen &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Single")) ||
            (fullScreen &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Single") &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Loop") &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Shuffle")) ||
            effectiveApplyRequest.display.standbyPresent) {
            return reject(
                QStringLiteral("UnsupportedReplaceConfiguration"),
                tr("Replace requires a fresh current layout that matches the selected preparation target and references the original media"));
        }
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = replace
        ? QStringLiteral("Preflight")
        : QStringLiteral("Converting");
    record.info.stage = replace
        ? QStringLiteral("ReadingReferences")
        : QStringLiteral("Converting");
    record.info.subject =
        artifact.metadata.remoteName;
    record.info.message = replace
        ? tr("Checking every device reference before replacement...")
        : tr("Preparing the recovered device copy as new media...");
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = printerProductId_;
    record.info.applyAfterUpload = replace;
    record.sourcePath = artifact.canonicalPath;
    record.sourceContentSha256 =
        artifact.metadata.decodedSha256;
    record.sourceSize =
        static_cast<qint64>(artifact.metadata.size);
    record.conversionProfile =
        paseRecoveredConversionProfile(profile);
    record.mediaConversion =
        printerMediaConversionIdentity(*productProfile, profile);
    record.mediaTransform = transform;
    record.mediaPreparationProfile = profile;
    record.sourceFingerprint =
        sourceFingerprint(record.sourcePath);
    record.artifactId = artifactId;
    record.artifactOwner = ownerUniqueName;
    record.artifactLeaseId = leaseId;
    record.requestedApplyFingerprint =
        requestedApplyFingerprint;
    record.recoveredSource = true;
    record.replaceOperation = replace;
    record.originalMediaId = originalMediaId;
    record.originalRemoteNameForReplace =
        originalEntry ? originalEntry->name : QString();
    record.applyRequest = effectiveApplyRequest;
    record.updateMetrics =
        effectiveApplyRequest.replaceOverlay;
    record.uploadDeviceIdentity =
        printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (replace) {
        emit requestBeginPrinterForegroundOperation(
            operationId, printerGeneration_);
        emit requestPrinterReplacePreflight(
            devicePath,
            record.originalRemoteNameForReplace,
            record.sourceSize,
            QString(), 0,
            operationId, printerGeneration_);
    } else {
        if (profile.target == QStringLiteral("SplitArea")) {
            emit requestPrepareRecoveredPrinterMediaWithPreparationProfile(
                operationId, devicePath, record.sourcePath,
                record.sourceContentSha256, printerGeneration_,
                record.mediaPreparationProfile,
                record.printerProductId);
        } else {
            emit requestPrepareRecoveredPrinterMedia(
                operationId, devicePath, record.sourcePath,
                record.sourceContentSha256, printerGeneration_,
                record.mediaTransform, record.printerProductId);
        }
    }
    return operationId;
}

QString DeviceManager::queueUploadOperation(
    const QString &requestedOperationId,
    const QString &localPath, bool applyAfterUpload,
    const TryxRuntimeApplyRequest &applyRequest,
    bool updateMetrics, bool ensureExisting,
    const TryxRuntimeMediaTransform &transform) {
    return queueUploadOperationWithPreparationProfile(
        requestedOperationId, localPath, applyAfterUpload,
        applyRequest, updateMetrics, ensureExisting,
        tryxFullFrameMediaPreparationProfile(transform));
}

QString DeviceManager::queueUploadWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &localPath,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return queueUploadOperationWithPreparationProfile(
        requestedOperationId, localPath, false,
        TryxRuntimeApplyRequest{}, false, false, profile);
}

QString DeviceManager::queueUploadOperationWithPreparationProfile(
    const QString &requestedOperationId,
    const QString &localPath, bool applyAfterUpload,
    const TryxRuntimeApplyRequest &applyRequest,
    bool updateMetrics, bool ensureExisting,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = ensureExisting
        ? QStringLiteral("EnsureMediaAndApply")
        : applyAfterUpload
            ? QStringLiteral("UploadAndApply")
            : QStringLiteral("Upload");
    const QString subject = QFileInfo(localPath).fileName();
    const QString inbox = mediaInboxDirectory();
    const QString managedRoot = inbox.isEmpty()
        ? QString()
        : QFileInfo(inbox).absolutePath();
    const QString canonicalSource =
        QFileInfo(localPath).canonicalFilePath();
    const bool quickStagedRequest =
        !managedRoot.isEmpty() &&
        (pathIsInside(localPath, managedRoot) ||
         (!canonicalSource.isEmpty() &&
          pathIsInside(canonicalSource, managedRoot)));
    const auto rejectedResult = [&]() {
        return quickStagedRequest ? QString() : operationId;
    };
    QString transformError;
    if (!tryxMediaPreparationProfileV1IsValid(
            profile, &transformError)) {
        const bool nestedTransformValid =
            tryxMediaTransformIsValid(profile.transform);
        if (!operations_.contains(operationId)) {
            rejectOperation(
                operationId, kind, subject,
                nestedTransformValid
                    ? QStringLiteral("InvalidMediaPreparationProfile")
                    : QStringLiteral("InvalidMediaTransform"),
                nestedTransformValid
                    ? tr("Media preparation profile is invalid: %1")
                          .arg(transformError)
                    : tr("Media transform is invalid: %1")
                          .arg(transformError));
        }
        return operations_.contains(operationId) &&
                       pathIsInside(
                           operations_.value(operationId).sourcePath,
                           mediaSpoolDirectory())
            ? operationId
            : rejectedResult();
    }
    if (operations_.contains(operationId)) {
        if (tryxMediaPreparationProfileFingerprint(
                operations_.value(operationId)
                    .mediaPreparationProfile) !=
            tryxMediaPreparationProfileFingerprint(profile)) {
            return {};
        }
        return quickStagedRequest &&
                       !pathIsInside(
                           operations_.value(operationId).sourcePath,
                           mediaSpoolDirectory())
            ? QString()
            : operationId;
    }

    if (firmwareExclusiveActive()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
        return rejectedResult();
    }
    if (retryCacheMutationGateActive()) {
        rejectOperation(
            operationId, kind, subject,
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
        return rejectedResult();
    }
    const std::optional<PrinterProductProfile> productProfile =
        currentPrinterProductProfile();
    if (!productProfile || !productProfile->mediaUploadSupported) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("UnsupportedProduct"),
            tr("Media upload is not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
        return rejectedResult();
    }
    if (profile.target == QStringLiteral("SplitArea") &&
        !productProfile->splitAreaMediaSupported) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("UnsupportedMediaPreparationTarget"),
            tr("Split-area media preparation is not supported for USB product %1")
                .arg(printerProductIdString(productProfile->productId)));
        return rejectedResult();
    }
    if ((applyAfterUpload &&
         !productProfile->displayConfigurationSupported) ||
        (ensureExisting && !productProfile->mediaCatalogSupported) ||
        (updateMetrics && !productProfile->overlayMetricsSupported)) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("UnsupportedProduct"),
            tr("This media workflow is not supported for USB product %1")
                .arg(printerProductIdString(productProfile->productId)));
        return rejectedResult();
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            operationId, kind, subject, QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return rejectedResult();
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, kind, subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return rejectedResult();
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return rejectedResult();
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return rejectedResult();
    }
    if (productProfile->mediaCatalogSupported &&
        printerDeviceSerial_.trimmed().isEmpty()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("DeviceIdentityUnavailable"),
            tr("PASE identity is unavailable; upload cannot start safely"));
        return rejectedResult();
    }
    const QFileInfo sourceInfo(localPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() ||
        sourceInfo.isSymLink()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("InvalidSource"),
                        tr("Media file does not exist"));
        return rejectedResult();
    }

    TryxRuntimeApplyRequest normalizedApplyRequest = applyRequest;
    if (normalizedApplyRequest.screenMode.isEmpty()) {
        normalizedApplyRequest.screenMode = QStringLiteral("Full Screen");
    }
    if (normalizedApplyRequest.playMode.isEmpty()) {
        normalizedApplyRequest.playMode = QStringLiteral("Single");
    }
    if (normalizedApplyRequest.ratio.isEmpty()) {
        normalizedApplyRequest.ratio = QStringLiteral("2:1");
    }
    if (normalizedApplyRequest.settingsColor.isEmpty()) {
        normalizedApplyRequest.settingsColor =
            QStringLiteral("#dcdcdc");
    }
    if (normalizedApplyRequest.settingsColor2.isEmpty()) {
        normalizedApplyRequest.settingsColor2 =
            normalizedApplyRequest.settingsColor;
    }
    if (applyAfterUpload) {
        normalizedApplyRequest.media.clear();
        if (normalizedApplyRequest.waterfallMode &&
            !normalizedApplyRequest.display.orientationPresent) {
            normalizedApplyRequest.display.orientationPresent = true;
            normalizedApplyRequest.display.waterfallMode = true;
        }
        const auto metricsAreValid = [](const QStringList &metrics) {
            return metrics.size() <= 3 &&
                   !hasDuplicateMetricLabels(metrics) &&
                   std::all_of(
                       metrics.cbegin(), metrics.cend(),
                       [](const QString &label) {
                           return isSupportedPaseMetricLabel(label);
                       });
        };
        const auto badgesAreValid = [](const QStringList &badges) {
            return badges.size() <= 2 &&
                   !hasDuplicateValues(badges) &&
                   std::all_of(
                       badges.cbegin(), badges.cend(),
                       [](const QString &badge) {
                           return isSupportedPaseBadge(badge);
                       });
        };
        const bool overlayRequested =
            updateMetrics || normalizedApplyRequest.replaceOverlay ||
            !normalizedApplyRequest.sysinfoLabels.isEmpty() ||
            !normalizedApplyRequest.settingsBadges.isEmpty();
        normalizedApplyRequest.replaceOverlay = overlayRequested;
        const bool overlayStyleValid =
            normalizeAndValidatePaseApplyOverlayStyles(
                &normalizedApplyRequest);
        if (normalizedApplyRequest.screenMode !=
                QStringLiteral("Full Screen") ||
            (normalizedApplyRequest.playMode != QStringLiteral("Single") &&
             normalizedApplyRequest.playMode != QStringLiteral("Loop") &&
             normalizedApplyRequest.playMode != QStringLiteral("Shuffle")) ||
            normalizedApplyRequest.ratio != QStringLiteral("2:1") ||
            !metricsAreValid(normalizedApplyRequest.sysinfoLabels) ||
            !badgesAreValid(normalizedApplyRequest.settingsBadges) ||
            !overlayStyleValid ||
            !normalizedApplyRequest.sysinfoLabels2.isEmpty() ||
            !normalizedApplyRequest.settingsBadges2.isEmpty() ||
            normalizedApplyRequest.display.standbyPresent ||
            (normalizedApplyRequest.display.brightnessPresent &&
             (normalizedApplyRequest.display.brightness < 0 ||
              normalizedApplyRequest.display.brightness > 100))) {
            rejectOperation(
                operationId, kind, subject,
                QStringLiteral("UnsupportedConfiguration"),
                tr("PASE upload-and-apply requires one full-screen media file, a supported play mode, up to three metrics and CPU/GPU badges"));
            return rejectedResult();
        }
    }

    QString effectiveSourcePath = sourceInfo.absoluteFilePath();
    bool ownsSourcePath = false;
    QString claimError;
    if (!claimQuickStagedSource(
            operationId, effectiveSourcePath,
            &effectiveSourcePath, &ownsSourcePath, &claimError)) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("InvalidStagedSource"),
            claimError.isEmpty()
                ? tr("The staged media source could not be claimed safely")
                : claimError);
        return rejectedResult();
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = ensureExisting
        ? QStringLiteral("Hashing")
        : QStringLiteral("Converting");
    record.info.stage = ensureExisting
        ? QStringLiteral("HashingSource")
        : QStringLiteral("Converting");
    record.info.subject = subject;
    record.info.message = ensureExisting
        ? tr("Calculating the source media content identity...")
        : tr("Preparing media for printer-class upload...");
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = productProfile->productId;
    record.info.applyAfterUpload = applyAfterUpload;
    record.uploadDeviceIdentity = printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration = printerGeneration_;
    record.applyRequest = normalizedApplyRequest;
    record.mediaTransform = profile.transform;
    record.mediaPreparationProfile = profile;
    record.updateMetrics =
        updateMetrics || normalizedApplyRequest.replaceOverlay;
    record.ensureExisting = ensureExisting;
    record.sourcePath = effectiveSourcePath;
    record.conversionProfile = printerConversionProfile(
        record.sourcePath, record.mediaPreparationProfile,
        record.printerProductId);
    record.mediaConversion =
        printerMediaConversionIdentity(
            *productProfile, record.mediaPreparationProfile);
    record.sourceFingerprint = sourceFingerprint(record.sourcePath);
    record.ownsSourcePath = ownsSourcePath;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (ensureExisting) {
        if (profile.target == QStringLiteral("SplitArea")) {
            emit requestAnalyzePrinterSourceWithPreparationProfile(
                operationId, record.sourcePath,
                printerGeneration_, record.mediaPreparationProfile,
                record.printerProductId);
        } else {
            emit requestAnalyzePrinterSource(
                operationId, record.sourcePath,
                printerGeneration_, record.mediaTransform,
                record.printerProductId);
        }
    } else {
        if (profile.target == QStringLiteral("SplitArea")) {
            emit requestPreparePrinterMediaWithPreparationProfile(
                operationId, devicePath, record.sourcePath,
                QString(), printerGeneration_,
                record.mediaPreparationProfile,
                record.printerProductId);
        } else {
            emit requestPreparePrinterMedia(
                operationId, devicePath, record.sourcePath,
                QString(), printerGeneration_,
                record.mediaTransform,
                record.printerProductId);
        }
    }
    return operationId;
}

QString DeviceManager::queueEnsureMediaAndApplyOperation(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &applyRequest,
    const TryxRuntimeMediaTransform &transform) {
    return queueUploadOperation(operationId, localPath, true,
                                applyRequest, true, true, transform);
}

QString DeviceManager::queueDeleteMediaOperation(
    const QString &requestedOperationId,
    const QStringList &fileNames) {
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = QStringLiteral("DeleteMedia");
    const QString subject = fileNames.join(QStringLiteral(", "));
    if (operations_.contains(operationId)) {
        return operationId;
    }
    if (firmwareExclusiveActive()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
        return operationId;
    }
    if (retryCacheMutationGateActive()) {
        rejectOperation(
            operationId, kind, subject,
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!currentPrinterSupportsMediaCatalog()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("UnsupportedProduct"),
            tr("Media deletion is not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
        return operationId;
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        QFileInfo::exists(deleteIntentPath())) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("DeleteReconciliationPending"),
            tr("A previous delete command still requires read-only reconciliation"));
        return operationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("Busy"),
                        tr("Another operation is active: %1")
                            .arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, kind, subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty() ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    if (fileNames.size() != 1) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("InvalidSelection"),
                        tr("Select exactly one media file to delete safely"));
        return operationId;
    }
    QSet<QString> seenNames;
    for (const QString &fileName : fileNames) {
        const auto catalogEntry = std::find_if(
            mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
            [&fileName](const TryxRuntimeMediaEntry &entry) {
                return entry.name == fileName;
            });
        if (!PrinterProtocol::isSafeUploadMediaName(fileName) ||
            seenNames.contains(fileName) ||
            catalogEntry == mediaCatalog_.entries.cend() ||
            !catalogEntry->deleteAllowed) {
            rejectOperation(
                operationId, kind, subject,
                QStringLiteral("DeleteNotAllowed"),
                tr("Media file is not eligible for deletion: %1")
                    .arg(fileName));
            return operationId;
        }
        seenNames.insert(fileName);
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("DeletePreflight");
    record.info.subject = subject;
    record.info.message = tr(
        "Preparing a safe delete operation...");
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = printerProductId_;
    record.deleteNames = fileNames;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    QString intentError;
    if (!writeDeleteIntent(operationId,
                           QStringLiteral("Preflight"), false, 0,
                           fileNames.constFirst(), {}, &intentError)) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("PersistenceFailed"), QString(),
            tr("Cannot persist delete intent before preflight: %1")
                .arg(intentError));
        return operationId;
    }
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterDeleteMedia(
        devicePath, fileNames, operationId, deleteIntentPath(), false,
        0, QString(), 0,
        printerGeneration_);
    return operationId;
}

QString DeviceManager::queueApplyOperation(const QString &requestedOperationId,
                                           const TryxRuntimeApplyRequest &request,
                                           bool updateMetrics,
                                           const QString &proofDeviceIdentity,
                                           const QList<TryxRuntimeSavedMediaRefV1> &proof,
                                           bool savedLayoutApply) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    TryxRuntimeApplyRequest normalizedRequest = request;
    if (!savedLayoutApply && normalizedRequest.screenMode.isEmpty()) {
        normalizedRequest.screenMode = QStringLiteral("Full Screen");
    }
    if (!savedLayoutApply && normalizedRequest.playMode.isEmpty()) {
        normalizedRequest.playMode = QStringLiteral("Single");
    }
    if (!savedLayoutApply && normalizedRequest.ratio.isEmpty()) {
        normalizedRequest.ratio = QStringLiteral("2:1");
    }
    if (!savedLayoutApply && normalizedRequest.settingsColor.isEmpty()) {
        normalizedRequest.settingsColor =
            QStringLiteral("#dcdcdc");
    }
    if (!savedLayoutApply && normalizedRequest.settingsColor2.isEmpty()) {
        normalizedRequest.settingsColor2 =
            normalizedRequest.settingsColor;
    }
    if (!savedLayoutApply && normalizedRequest.waterfallMode &&
        !normalizedRequest.display.orientationPresent) {
        normalizedRequest.display.orientationPresent = true;
        normalizedRequest.display.waterfallMode = true;
    }
    const QString presetMedia =
        printerPresetMediaFile(normalizedRequest.presetId);
    if (!savedLayoutApply && normalizedRequest.media.isEmpty() &&
        !presetMedia.isEmpty()) {
        normalizedRequest.media = {presetMedia};
    }
    const bool hasMediaChange = !normalizedRequest.media.isEmpty();
    const bool hasDisplayChange =
        normalizedRequest.display.brightnessPresent ||
        normalizedRequest.display.standbyPresent ||
        normalizedRequest.display.backlightPresent ||
        normalizedRequest.display.orientationPresent;
    const bool overlayRequested =
        updateMetrics || normalizedRequest.replaceOverlay ||
        !normalizedRequest.sysinfoLabels.isEmpty() ||
        !normalizedRequest.settingsBadges.isEmpty() ||
        !normalizedRequest.sysinfoLabels2.isEmpty() ||
        !normalizedRequest.settingsBadges2.isEmpty();
    normalizedRequest.replaceOverlay = overlayRequested;
    const bool overlayStyleValid =
        normalizeAndValidatePaseApplyOverlayStyles(
            &normalizedRequest);
    QStringList subjectMedia;
    for (const QString &mediaFile : normalizedRequest.media) {
        subjectMedia.append(printerMediaConfigName(mediaFile));
    }
    const QString subject = hasMediaChange
        ? subjectMedia.join(QStringLiteral(" + "))
        : tr("Display settings");
    if (operations_.contains(operationId)) {
        return operationId;
    }
    const auto rejectApply =
        [this, &operationId, &subject, savedLayoutApply](
            const QString &category, const QString &message) {
            if (savedLayoutApply) {
                rejectSavedLayoutApplyOperation(
                    operationId, subject, category, message);
            } else {
                rejectOperation(
                    operationId, QStringLiteral("Apply"), subject,
                    category, message);
            }
        };

    if (firmwareExclusiveActive()) {
        rejectApply(
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
        return operationId;
    }
    if (retryCacheMutationGateActive()) {
        rejectApply(
            savedLayoutApply || retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!currentPrinterSupportsDisplayConfiguration() ||
        (overlayRequested && !currentPrinterSupportsOverlayMetrics())) {
        rejectApply(
            QStringLiteral("UnsupportedProduct"),
            tr("Display configuration is not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
        return operationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectApply(
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectApply(
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectApply(
            savedLayoutApply
                ? QStringLiteral("SessionLost")
                : QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectApply(savedLayoutApply
                        ? QStringLiteral("SessionLost")
                        : QStringLiteral("DeviceUnavailable"),
                    printerUnavailableStatusText());
        return operationId;
    }
    const auto metricsAreValid = [](const QStringList &metrics) {
        return metrics.size() <= 3 &&
               !hasDuplicateMetricLabels(metrics) &&
               std::all_of(
                   metrics.cbegin(), metrics.cend(),
                   [](const QString &label) {
                       return isSupportedPaseMetricLabel(label);
                   });
    };
    const auto badgesAreValid = [](const QStringList &badges) {
        return badges.size() <= 2 &&
               !hasDuplicateValues(badges) &&
               std::all_of(
                   badges.cbegin(), badges.cend(),
                   [](const QString &badge) {
                       return isSupportedPaseBadge(badge);
                   });
    };
    const bool fullScreen =
        normalizedRequest.screenMode == QStringLiteral("Full Screen");
    const bool splitScreen =
        normalizedRequest.screenMode ==
        QStringLiteral("Screen Splitting");
    const bool playModeValid =
        splitScreen
        ? normalizedRequest.playMode == QStringLiteral("Single")
        : normalizedRequest.playMode == QStringLiteral("Single") ||
              normalizedRequest.playMode == QStringLiteral("Loop") ||
              normalizedRequest.playMode == QStringLiteral("Shuffle");
    const bool mediaCountValid =
        !hasMediaChange ||
        (fullScreen && normalizedRequest.media.size() == 1) ||
        (splitScreen && normalizedRequest.media.size() == 2);
    const bool rightOverlayValid =
        splitScreen ||
        (normalizedRequest.sysinfoLabels2.isEmpty() &&
         normalizedRequest.settingsBadges2.isEmpty());
    if ((!hasMediaChange && !hasDisplayChange &&
         !normalizedRequest.replaceOverlay) ||
        (!fullScreen && !splitScreen) || !playModeValid ||
        !mediaCountValid ||
        normalizedRequest.ratio != QStringLiteral("2:1") ||
        !metricsAreValid(normalizedRequest.sysinfoLabels) ||
        !metricsAreValid(normalizedRequest.sysinfoLabels2) ||
        !badgesAreValid(normalizedRequest.settingsBadges) ||
        !badgesAreValid(normalizedRequest.settingsBadges2) ||
        !overlayStyleValid ||
        !rightOverlayValid ||
        normalizedRequest.display.standbyPresent ||
        (normalizedRequest.display.brightnessPresent &&
         (normalizedRequest.display.brightness < 0 ||
          normalizedRequest.display.brightness > 100))) {
        rejectApply(
            QStringLiteral("UnsupportedConfiguration"),
            tr("PASE configuration requires a display change or valid full/split media, supported play mode, up to three metrics per side and CPU/GPU badges"));
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = savedLayoutApply
        ? QStringLiteral("SavedLayoutApply")
        : QStringLiteral("Apply");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.resultName = subject;
    record.info.message = hasMediaChange
        ? tr("Preparing to apply printer-class media...")
        : tr("Preparing to apply printer-class display settings...");
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = printerProductId_;
    record.mediaFile = hasMediaChange
        ? printerMediaConfigName(normalizedRequest.media.constFirst())
        : QString();
    record.applyRequest = normalizedRequest;
    record.updateMetrics = normalizedRequest.replaceOverlay;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterApplyMedia(devicePath, record.mediaFile,
                                  record.applyRequest,
                                  record.updateMetrics,
                                  proofDeviceIdentity, proof,
                                  operationId, printerGeneration_);
    return operationId;
}

QString DeviceManager::queueSavedLayoutApplyOperation(
    const QString &requestedOperationId, const QString &layoutId,
    quint64 expectedLayoutRevision,
    const TryxRuntimeApplyRequest &currentDraft) {
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    if (operations_.contains(operationId)) {
        return operationId;
    }
    const auto reject =
        [this, &operationId, &layoutId](
            const QString &category, const QString &message) {
            rejectSavedLayoutApplyOperation(
                operationId, layoutId, category, message);
            return operationId;
        };
    if (runtimeDowngradeV10Prepared_) {
        return reject(
            QStringLiteral("DowngradePrepared"),
            tr("Saved layout Apply is blocked after runtime downgrade preparation"));
    }

    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        savedLayoutsSnapshot();
    if (snapshot.status != QStringLiteral("Ready")) {
        return reject(
            snapshot.status == QStringLiteral("Unsupported")
                ? QStringLiteral("UnsupportedProduct")
                : QStringLiteral("SavedLayoutsUnavailable"),
            snapshot.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : snapshot.diagnostic);
    }

    auto found = std::find_if(
        snapshot.layouts.cbegin(), snapshot.layouts.cend(),
        [&layoutId](const TryxRuntimeSavedLayoutV1 &layout) {
            return layout.layoutId == layoutId;
    });
    if (found == snapshot.layouts.cend()) {
        const QList<TryxRuntimeSavedLayoutV1> allLayouts =
            savedLayoutStore_->layouts();
        const auto foreign = std::find_if(
            allLayouts.cbegin(), allLayouts.cend(),
            [&layoutId](const TryxRuntimeSavedLayoutV1 &layout) {
                return layout.layoutId == layoutId;
            });
        if (foreign != allLayouts.cend()) {
            return reject(
                QStringLiteral("DeviceIdentityChanged"),
                tr("The saved layout belongs to another connected device"));
        }
        return reject(
            QStringLiteral("SavedLayoutMissing"),
            tr("The saved layout no longer exists"));
    }
    if (found->revision != expectedLayoutRevision) {
        return reject(
            QStringLiteral("SavedLayoutRevisionConflict"),
            tr("The saved layout changed; load it again before applying"));
    }

    QList<TryxRuntimeSavedMediaRefV1> proof;
    QString validationError;
    if (!buildSavedLayoutMediaProof(
            currentDraft, &proof, &validationError)) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            validationError.isEmpty()
                ? tr("The saved layout media is unavailable")
                : validationError);
    }
    TryxRuntimeSavedLayoutV1 candidate = *found;
    candidate.media = proof;
    candidate.request = currentDraft;
    if (!tryx::SavedLayoutStore::layoutIsCanonical(
            candidate, &validationError)) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            validationError.isEmpty()
                ? tr("The current saved layout draft is invalid")
                : validationError);
    }

    return queueApplyOperation(
        operationId, currentDraft, false,
        snapshot.deviceIdentity, proof, true);
}

void DeviceManager::rejectCacheCleanupOperation(
    const QString &operationId, const QString &category,
    const QString &message) {
    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("CacheCleanup");
    record.info.state = QStringLiteral("Failed");
    record.info.stage = QStringLiteral("Rejected");
    record.info.errorCategory = category;
    record.info.terminalOutcome = QStringLiteral("NotStarted");
    record.info.subject = tr("Temporary files");
    record.info.message = message;
    record.info.deviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::releaseCacheCleanupLatch() {
    cacheCleanupOperationId_.clear();
    cacheCleanupExclusiveActive_ = false;

    const bool applyDeferredCatalog =
        deferredMediaCatalogUpdatePending_;
    const QList<PrinterProtocol::MediaFile> deferredCatalog =
        deferredMediaCatalogFiles_;
    const quint64 deferredGeneration =
        deferredMediaCatalogGeneration_;
    const QString deferredIdentity =
        deferredMediaCatalogDeviceIdentity_;
    deferredMediaCatalogFiles_.clear();
    deferredMediaCatalogGeneration_ = 0;
    deferredMediaCatalogDeviceIdentity_.clear();
    deferredMediaCatalogUpdatePending_ = false;

    if (applyDeferredCatalog) {
        QTimer::singleShot(
            0, this,
            [this, deferredCatalog, deferredGeneration,
             deferredIdentity]() {
                if (printerGeneration_ == deferredGeneration &&
                    printerDeviceSerial_.trimmed() == deferredIdentity) {
                    updateMediaCatalog(deferredCatalog);
                }
            });
    }
    QTimer::singleShot(
        0, this, [this]() { sweepDeviceMediaArtifacts(); });
}

void DeviceManager::finishCacheCleanupOperation(
    const QString &operationId, const QString &state,
    const QString &errorCategory, const QString &terminalOutcome,
    const QString &message) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        found->info.kind != QStringLiteral("CacheCleanup")) {
        return;
    }
    found->info.state = state;
    found->info.stage = state == QStringLiteral("Succeeded")
        ? QStringLiteral("Succeeded")
        : state == QStringLiteral("Cancelled")
            ? QStringLiteral("Cancelled")
            : QStringLiteral("Failed");
    found->info.errorCategory = errorCategory;
    found->info.terminalOutcome = terminalOutcome;
    found->info.retryMode.clear();
    found->info.resultName.clear();
    found->info.message = message;
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    publishOperation(operationId);
    releaseCacheCleanupLatch();
    pruneOperationHistory();
}

QString DeviceManager::queueCacheCleanupOperation(
    const QString &requestedOperationId, QString *errorName,
    QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto invalidOperation = [errorName, errorMessage](
                                      const QString &message) {
        if (errorName) {
            *errorName = QStringLiteral(
                "org.tryx.Panorama.Error.InvalidOperation");
        }
        if (errorMessage) {
            *errorMessage = message;
        }
        return QString();
    };
    const QUuid parsedOperationId(requestedOperationId);
    if (parsedOperationId.isNull() ||
        parsedOperationId.toString(QUuid::WithoutBraces) !=
            requestedOperationId) {
        return invalidOperation(tr(
            "The cache cleanup operation ID is invalid"));
    }
    const QString operationId = requestedOperationId;
    const auto existing = operations_.constFind(operationId);
    if (existing != operations_.constEnd()) {
        if (existing->info.kind == QStringLiteral("CacheCleanup")) {
            return operationId;
        }
        return invalidOperation(tr(
            "The operation ID is already used by another operation kind"));
    }

    const auto reject = [this, &operationId](
                            const QString &category,
                            const QString &message) {
        rejectCacheCleanupOperation(operationId, category, message);
        return operationId;
    };
    if (runtimeDowngradeV10Prepared_) {
        return reject(
            QStringLiteral("DowngradePrepared"),
            tr("Temporary file cleanup is blocked after runtime downgrade preparation"));
    }
    if (firmwareExclusiveActive() ||
        !firmwareReleasePendingLeaseId_.isEmpty() ||
        firmwareRecoveryInterlockActive_) {
        return reject(
            QStringLiteral("FirmwareUpdateActive"),
            tr("Firmware update or recovery must finish before temporary files can be removed"));
    }
    if (cacheCleanupExclusiveActive_ || !activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tr("Another operation is active"));
    }
    bool retryAvailableOperationPresent = false;
    for (auto operation = operations_.cbegin();
         operation != operations_.cend(); ++operation) {
        if (operation->info.state == QStringLiteral("RetryAvailable")) {
            retryAvailableOperationPresent = true;
            continue;
        }
        if (!operationIsTerminal(operation->info.state)) {
            return reject(
                QStringLiteral("Busy"),
                tr("Another operation is still pending"));
        }
    }
    if (printerRecoveryRequired_) {
        return reject(
            QStringLiteral("DeviceRecoveryRequired"),
            tr("Device recovery must finish before temporary files can be removed"));
    }
    if (printerDisplaySessionLost_) {
        return reject(
            QStringLiteral("SessionLost"),
            tr("The lost device session must be resolved before temporary files can be removed"));
    }
    if (!retryCacheLoadComplete_ ||
        !pendingRetryCacheValidations_.isEmpty()) {
        return reject(
            QStringLiteral("RetryCacheValidationPending"),
            tr("Stored retry media is still being validated"));
    }
    if (retryCacheStartupFailure_ || !retryCacheStore_) {
        return reject(
            retryCacheStore_
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("CacheUnavailable"),
            retryCacheFailureDetail_.isEmpty()
                ? tr("Stored retry media could not be validated safely")
                : retryCacheFailureDetail_);
    }
    if (retryAvailableOperationPresent ||
        retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        !retryCacheSnapshot_.cleanupPending.isEmpty() ||
        retryCacheSnapshot_.candidateTransition.has_value() ||
        retryCacheStore_->blocksMutations()) {
        return reject(
            QStringLiteral("RetryCacheConflict"),
            tr("Stored retry media must be resolved before temporary files can be removed"));
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        pendingDeleteIntent_.has_value() ||
        filesystemLeafExistsOrIsAmbiguous(deleteIntentPath()) ||
        !pendingReplaceJournalOperationId_.isEmpty() ||
        filesystemLeafExistsOrIsAmbiguous(replaceIntentPath())) {
        return reject(
            QStringLiteral("RecoveryJournalPresent"),
            tr("Delete or replacement recovery must finish before temporary files can be removed"));
    }
    if (!mediaCatalogStore_ || !deviceMediaArtifactStore_) {
        return reject(
            QStringLiteral("CacheUnavailable"),
            tr("Temporary file storage is unavailable"));
    }

    // All runtime entry points execute on this thread. Publish the latch before
    // constructing either store plan so no timer, owner callback or direct
    // artifact lease call can interleave with the immutable assessment.
    cacheCleanupExclusiveActive_ = true;
    cacheCleanupOperationId_ = operationId;
    const auto releaseLatch = [this]() {
        releaseCacheCleanupLatch();
    };

    const auto artifactAssessment =
        deviceMediaArtifactStore_->cleanupAssessment();
    if (!artifactAssessment.ok()) {
        const auto code = artifactAssessment.result.code;
        if (code == tryx::DeviceMediaArtifactStore::ErrorCode::Busy) {
            const QString rejected = reject(
                QStringLiteral("ArtifactLeaseActive"),
                artifactAssessment.result.detail);
            releaseLatch();
            return rejected;
        }
        if (code != tryx::DeviceMediaArtifactStore::ErrorCode::
                        PlanLimitExceeded) {
            const QString rejected = reject(
                QStringLiteral("CacheUnavailable"),
                artifactAssessment.result.detail);
            releaseLatch();
            return rejected;
        }
        OperationRecord failed;
        failed.info.id = operationId;
        failed.info.kind = QStringLiteral("CacheCleanup");
        failed.info.state = QStringLiteral("Failed");
        failed.info.stage = QStringLiteral("Failed");
        failed.info.errorCategory =
            QStringLiteral("CacheCleanupFailed");
        failed.info.terminalOutcome = QStringLiteral("NotStarted");
        failed.info.subject = tr("Temporary files");
        failed.info.message = artifactAssessment.result.detail;
        failed.info.deviceGeneration = printerGeneration_;
        operations_.insert(operationId, failed);
        operationOrder_.append(operationId);
        publishOperation(operationId);
        releaseLatch();
        pruneOperationHistory();
        return operationId;
    }

    const auto catalogPlan =
        mediaCatalogStore_->planThumbnailOrphanCleanup();
    if (!catalogPlan.ok()) {
        const bool boundedFailure =
            catalogPlan.result.code ==
            tryx::MediaCatalogStore::ErrorCode::PlanLimitExceeded;
        if (!boundedFailure) {
            const QString rejected = reject(
                QStringLiteral("CacheUnavailable"),
                catalogPlan.result.detail);
            releaseLatch();
            return rejected;
        }
        OperationRecord failed;
        failed.info.id = operationId;
        failed.info.kind = QStringLiteral("CacheCleanup");
        failed.info.state = QStringLiteral("Failed");
        failed.info.stage = QStringLiteral("Failed");
        failed.info.errorCategory =
            QStringLiteral("CacheCleanupFailed");
        failed.info.terminalOutcome = QStringLiteral("NotStarted");
        failed.info.subject = tr("Temporary files");
        failed.info.message = catalogPlan.result.detail;
        failed.info.deviceGeneration = printerGeneration_;
        operations_.insert(operationId, failed);
        operationOrder_.append(operationId);
        publishOperation(operationId);
        releaseLatch();
        pruneOperationHistory();
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("CacheCleanup");
    record.info.state = QStringLiteral("Running");
    record.info.stage = QStringLiteral("CleaningCatalog");
    record.info.subject = tr("Temporary files");
    record.info.message = tr("Removing unused temporary files...");
    record.info.deviceGeneration = printerGeneration_;
    record.info.total = catalogPlan.plannedFiles +
        artifactAssessment.plan.plannedFiles;
    record.cacheCatalogPlan = catalogPlan;
    record.cacheArtifactPlan = artifactAssessment.plan;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    QTimer::singleShot(
        0, this,
        [this, operationId]() { continueCacheCleanup(operationId); });
    return operationId;
}

void DeviceManager::continueCacheCleanup(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (!cacheCleanupExclusiveActive_ ||
        cacheCleanupOperationId_ != operationId ||
        activeOperationId_ != operationId ||
        found == operations_.end() ||
        found->info.kind != QStringLiteral("CacheCleanup") ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    if (found->cancelRequested) {
        const bool partial = found->info.completed > 0;
        if (!partial) {
            found->info.total = 0;
            found->info.confirmedBytes = 0;
        }
        finishCacheCleanupOperation(
            operationId,
            QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"),
            partial ? QStringLiteral("PartialCleanup")
                    : QStringLiteral("Cancelled"),
            partial
                ? tr("Temporary file cleanup was cancelled after some files were removed")
                : tr("Temporary file cleanup was cancelled"));
        return;
    }

    const auto fail = [this, &found, &operationId](const QString &detail) {
        const bool partial = found->info.completed > 0;
        if (!partial) {
            found->info.total = 0;
            found->info.confirmedBytes = 0;
        }
        finishCacheCleanupOperation(
            operationId,
            QStringLiteral("Failed"),
            QStringLiteral("CacheCleanupFailed"),
            partial ? QStringLiteral("PartialCleanup")
                    : QStringLiteral("NotStarted"),
            detail.isEmpty()
                ? tr("Temporary file cleanup failed")
                : detail);
    };

    constexpr qsizetype kCleanupBatchSize = 16;
    if (found->cacheCatalogIndex <
        found->cacheCatalogPlan.candidates.size()) {
        const auto batch =
            mediaCatalogStore_->cleanupThumbnailOrphanBatch(
                found->cacheCatalogPlan,
                found->cacheCatalogIndex, kCleanupBatchSize);
        found->info.completed += batch.removedFiles;
        found->info.confirmedBytes += batch.removedLogicalBytes;
        found->cacheCatalogIndex = batch.nextIndex;
        if (!batch.ok()) {
            fail(batch.result.detail);
            return;
        }
        publishOperation(operationId);
        QTimer::singleShot(
            0, this,
            [this, operationId]() {
                continueCacheCleanup(operationId);
            });
        return;
    }

    if (found->cacheArtifactIndex <
        found->cacheArtifactPlan.candidates.size()) {
        found->info.stage = QStringLiteral("CleaningArtifacts");
        const auto batch = deviceMediaArtifactStore_->cleanupBatch(
            found->cacheArtifactPlan,
            found->cacheArtifactIndex, kCleanupBatchSize);
        found->info.completed += batch.removedFiles;
        found->info.confirmedBytes += batch.removedLogicalBytes;
        found->cacheArtifactIndex = batch.nextIndex;
        if (artifactOwnerWatcher_) {
            for (const QString &owner : batch.ownersNoLongerUsed) {
                artifactOwnerWatcher_->removeWatchedService(owner);
            }
        }
        if (!batch.ok()) {
            fail(batch.result.detail);
            return;
        }
        publishOperation(operationId);
        QTimer::singleShot(
            0, this,
            [this, operationId]() {
                continueCacheCleanup(operationId);
            });
        return;
    }

    const bool empty = found->info.total == 0;
    finishCacheCleanupOperation(
        operationId,
        QStringLiteral("Succeeded"), QString(),
        QStringLiteral("Succeeded"),
        empty
            ? tr("No safe temporary files were found")
            : tr("Unused temporary files were removed"));
}

QString DeviceManager::queueMetricsConfigOperation(
    const QString &requestedOperationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString subject = request.enabled
        ? request.metrics.join(QStringLiteral(", "))
        : tr("Disabled");
    if (operations_.contains(operationId)) {
        return operationId;
    }
    if (firmwareExclusiveActive()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
        return operationId;
    }
    if (retryCacheMutationGateActive()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!currentPrinterSupportsOverlayMetrics()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("UnsupportedProduct"),
            tr("Overlay metrics are not supported for USB product %1")
                .arg(printerProductIdString(printerProductId_)));
        return operationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectOperation(operationId, QStringLiteral("MetricsConfig"),
                        subject, QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    const bool alignmentValid =
        request.alignment == QStringLiteral("Left") ||
        request.alignment == QStringLiteral("Center") ||
        request.alignment == QStringLiteral("Right");
    const bool unsupportedMetric = std::any_of(
        request.metrics.cbegin(), request.metrics.cend(),
        [](const QString &label) {
            return !isSupportedPaseMetricLabel(label);
        });
    const bool requestValid = alignmentValid &&
        request.textColor <= 0x00FFFFFFU &&
        !hasDuplicateMetricLabels(request.metrics) &&
        ((request.enabled && !request.metrics.isEmpty() &&
          request.metrics.size() <= 3 && !unsupportedMetric) ||
         (!request.enabled && request.metrics.isEmpty()));
    if (!requestValid) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("UnsupportedConfiguration"),
            tr("PASE metrics configuration requires one to three unique supported metrics, or an explicit disabled state"));
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("MetricsConfig");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.message = request.enabled
        ? tr("Preparing to configure PASE metrics...")
        : tr("Preparing to disable PASE metrics...");
    record.info.deviceGeneration = printerGeneration_;
    record.printerProductId = printerProductId_;
    record.metricsRequest = request;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterConfigureMetrics(devicePath, request, operationId,
                                        printerGeneration_);
    return operationId;
}

QString DeviceManager::retryOperation(const QString &sourceOperationId,
                                      const QString &requestedNewOperationId) {
    const QString newOperationId =
        normalizedOperationId(requestedNewOperationId);
    if (newOperationId.isEmpty()) {
        return {};
    }
    if (operations_.contains(newOperationId)) {
        return newOperationId;
    }
    if (firmwareExclusiveActive()) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            QString(), QStringLiteral("FirmwareUpdateActive"),
            firmwareExclusiveStatusText());
        return newOperationId;
    }
    if (retryCacheMutationGateActive()) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"), QString(),
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            printerMutationUnavailableStatusText());
        return newOperationId;
    }
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value()) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        QString(), QStringLiteral("RetryUnavailable"),
                        tr("There is no durable prepared-media retry candidate"));
        return newOperationId;
    }
    const auto candidate = *retryCacheSnapshot_.retryCandidate;
    const auto source = operations_.constFind(sourceOperationId);
    if (source == operations_.constEnd() ||
        sourceOperationId != candidate.operationId ||
        source->info.state != QStringLiteral("RetryAvailable") ||
        source->info.retryMode != QStringLiteral("PreparedMedia") ||
        source->uploadFinalizationReconciliationPending ||
        source->retryLineageId != candidate.lineageId ||
        source->retryDispatchId != candidate.dispatchId ||
        source->printerProductId != candidate.productId ||
        source->uploadDeviceIdentity != candidate.deviceIdentity ||
        source->uploadDeviceGeneration != candidate.deviceGeneration ||
        source->preparedPath != retryCacheArtifactPath(candidate.prepared) ||
        source->preparedSha256 != candidate.prepared.sha256) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        QString(), QStringLiteral("RetryUnavailable"),
                        tr("This operation has no safe prepared-media retry"));
        return newOperationId;
    }
    if (!operationMatchesCurrentPrinterProduct(*source)) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            QStringLiteral("DeviceProfileMismatch"),
            tr("Prepared media belongs to USB product %1, but the connected device is %2")
                .arg(printerProductIdString(source->printerProductId),
                     printerProductIdString(printerProductId_)));
        return newOperationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"), source->info.subject,
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return newOperationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_ ||
        source->requiresDeviceRecovery) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            (printerRecoveryRequired_ || source->requiresDeviceRecovery)
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return newOperationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return newOperationId;
    }
    if (currentPrinterPath().isEmpty()) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        source->info.subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return newOperationId;
    }
    const bool identityRequired =
        source->retryMustUseNewRemoteName ||
        source->info.terminalOutcome ==
            QStringLiteral("PartialOrUnknown") ||
        source->info.terminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    const QString expectedDeviceIdentity =
        source->uploadDeviceIdentity.trimmed();
    const QString currentDeviceIdentity =
        printerDeviceSerial_.trimmed();
    if (identityRequired &&
        (expectedDeviceIdentity.isEmpty() ||
         currentDeviceIdentity.isEmpty() ||
         expectedDeviceIdentity != currentDeviceIdentity)) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            QStringLiteral("DeviceIdentityMismatch"),
            tr("Prepared media belongs to a different or unverified PASE connection. Reconnect the original device before Retry."));
        return newOperationId;
    }
    const OperationRecord sourceRecord = source.value();
    OperationRecord record = sourceRecord;
    if (record.replaceOperation) {
        // A prepared-media retry may upload a new copy, but it must never
        // resume the Apply/Delete portion of a previous Replace saga.
        record.replaceOperation = false;
        record.replaceJournalActive = false;
        record.replaceJournal = {};
        record.originalMediaId.clear();
        record.originalRemoteNameForReplace.clear();
        record.replaceReferences.clear();
        record.replaceReferenceSlots.clear();
        record.info.applyAfterUpload = false;
        record.applyRequest = {};
        record.updateMetrics = false;
    }
    record.info.id = newOperationId;
    record.info.parentId = sourceOperationId;
    record.info.kind = QStringLiteral("UploadRetry");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = currentPrinterSupportsMediaCatalog()
        ? QStringLiteral("RefreshingMedia")
        : QStringLiteral("EnsuringSession");
    record.info.errorCategory.clear();
    record.info.retryMode.clear();
    record.info.message = currentPrinterSupportsMediaCatalog()
        ? tr("Checking the device file list before manual retry...")
        : tr("Preparing the durable manual retry dispatch...");
    record.info.completed = 0;
    record.info.confirmedBytes = 0;
    record.info.lastConfirmedChunkIndex = -1;
    record.info.attempt = candidate.attempt + 1;
    record.info.deviceGeneration = printerGeneration_;
    record.uploadDeviceIdentity = currentDeviceIdentity;
    record.uploadDeviceGeneration = printerGeneration_;
    record.cancelRequested = false;
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.retryPreflight = currentPrinterSupportsMediaCatalog();
    record.uploadDispatched = false;
    record.ownsSourcePath = false;
    record.retryLineageId = candidate.lineageId;
    record.retryDispatchId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    record.uploadFinalizationReconciliationPending = false;
    record.requiresDeviceRecovery = false;
    record.retryMustUseNewRemoteName =
        candidate.requiresNewRemoteName;
    if (record.retryMustUseNewRemoteName &&
        !currentPrinterSupportsMediaCatalog()) {
        const QString originalName = record.originalRemoteName.isEmpty()
            ? record.remoteName
            : record.originalRemoteName;
        const QString originalSuffix = originalName.contains(
            QStringLiteral(".png.h264_"))
            ? QStringLiteral("png")
            : originalName.contains(QStringLiteral(".gif.h264_"))
                ? QStringLiteral("gif")
                : QStringLiteral("mp4");
        record.remoteName = h264PrinterNameForConversion(
            generatedPrinterMediaName(originalSuffix),
            record.printerProductId,
            record.mediaConversion);
    }
    record.info.resultName = record.remoteName;
    operations_.insert(newOperationId, record);
    operationOrder_.append(newOperationId);
    activeOperationId_ = newOperationId;
    publishOperation(newOperationId);
    emit requestBeginPrinterForegroundOperation(
        newOperationId, printerGeneration_);
    if (record.retryPreflight) {
        emit requestPrinterRefreshMedia(
            currentPrinterPath(), newOperationId,
            printerGeneration_);
    } else {
        dispatchPreparedUploadWithRetryBarrier(
            currentPrinterPath(), newOperationId,
            printerGeneration_);
    }
    return newOperationId;
}

void DeviceManager::cancelOperation(const QString &operationId) {
    if (runtimeDowngradeV10Prepared_) {
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    if (found->info.state == QStringLiteral("RetryAvailable") &&
        activeOperationId_ != operationId) {
        if (found->info.retryMode ==
            QStringLiteral("DeleteReconcile")) {
            found->info.message = tr(
                "Delete reconciliation cannot be cancelled because FileRemove may already have been sent");
            publishOperation(operationId);
            return;
        }
        if (!clearRetryCacheCandidate(operationId)) {
            found = operations_.find(operationId);
            if (found != operations_.end()) {
                found->info.errorCategory =
                    QStringLiteral("RetryCacheCleanupFailed");
                const bool durableCleanupPending =
                    !retryCacheSnapshot_.cleanupPending.isEmpty() &&
                    !retryCacheSnapshot_.retryCandidate.has_value() &&
                    !retryCacheSnapshot_.inFlightDispatch.has_value();
                if (durableCleanupPending) {
                    found->info.state = QStringLiteral("Cancelled");
                    found->info.stage = QStringLiteral("Cancelled");
                    found->info.terminalOutcome =
                        QStringLiteral("Cancelled");
                    found->info.retryMode.clear();
                    found->info.message = tr(
                        "The retry was removed, but its local files still require bounded cleanup; device mutations remain blocked");
                } else {
                    found->info.message = tr(
                        "The retry cache could not be removed; it remains available");
                }
                publishOperation(operationId);
            }
        }
        return;
    }
    if (activeOperationId_ != operationId ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    found->cancelRequested = true;
    if (found->info.kind == QStringLiteral("CacheCleanup")) {
        found->info.message = tr(
            "Cancelling temporary file cleanup...");
        publishOperation(operationId);
        continueCacheCleanup(operationId);
        return;
    }
    if (found->info.state == QStringLiteral("Converting") ||
        found->info.state == QStringLiteral("Hashing")) {
        emit requestCancelPrinterPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("UserCancelled"), QString(),
                        tr("Operation cancelled by the user"));
        return;
    }
    found->info.message = tr("Cancelling the active USB operation...");
    publishOperation(operationId);
    worker_->cancelPrinterOperation(operationId);
}

void DeviceManager::cancelForegroundForGenerationChange(
    const QString &message) {
    if (activeOperationId_.isEmpty() ||
        !operations_.contains(activeOperationId_)) {
        return;
    }
    const QString operationId = activeOperationId_;
    OperationRecord &record = operations_[operationId];
    if (record.info.state == QStringLiteral("Converting") ||
        record.info.state == QStringLiteral("Hashing")) {
        emit requestCancelPrinterPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("DeviceChanged"), QString(), message);
        return;
    }
    record.deviceChangePending = true;
    record.deviceChangeMessage = message;
    record.info.message = message;
    publishOperation(operationId);
}

void DeviceManager::handlePreparedUploadFailure(
    const QString &operationId, const QString &message,
    PrinterProtocol::MutationOutcome outcome) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    const bool finalizationUnknown =
        outcome == PrinterProtocol::MutationOutcome::FinalizationUnknown ||
        found->uploadFinalizationReconciliationPending ||
        found->info.terminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    const bool retryableOutcome =
        finalizationUnknown ||
        outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown ||
        outcome == PrinterProtocol::MutationOutcome::VerificationFailed;
    const QString terminalOutcome = finalizationUnknown
        ? QStringLiteral("FinalizationUnknown")
        : retryableOutcome
            ? QStringLiteral("PartialOrUnknown")
            : mutationOutcomeName(outcome);
    if (found->info.primaryErrorCategory.isEmpty()) {
        found->info.primaryErrorCategory = terminalOutcome;
    }
    if (found->info.primaryErrorMessage.isEmpty()) {
        found->info.primaryErrorMessage = message;
    }

    const bool currentDispatch =
        retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->operationId == operationId;
    if (currentDispatch &&
        retryCacheSnapshot_.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed) {
        QString storeError;
        bool durable = false;
        if (retryableOutcome) {
            const qint64 confirmedBytes = finalizationUnknown
                ? retryCacheSnapshot_.inFlightDispatch->prepared.size
                : qBound<qint64>(
                      0, found->info.confirmedBytes,
                      retryCacheSnapshot_.inFlightDispatch->prepared.size);
            durable = recordRetryCacheOutcome(
                operationId,
                finalizationUnknown
                    ? tryx::RetryCacheStore::TerminalOutcome::
                          FinalizationUnknown
                    : tryx::RetryCacheStore::TerminalOutcome::
                          PartialOrUnknown,
                confirmedBytes, found->info.primaryErrorCategory,
                found->info.primaryErrorMessage, &storeError);
        } else {
            tryx::RetryCacheStore::DispatchRetirement retirement =
                tryx::RetryCacheStore::DispatchRetirement::
                    ProvenNotStarted;
            if (outcome == PrinterProtocol::MutationOutcome::Rejected) {
                retirement = tryx::RetryCacheStore::
                    DispatchRetirement::ProvenRejected;
            } else if (outcome ==
                       PrinterProtocol::MutationOutcome::Cancelled) {
                retirement = tryx::RetryCacheStore::
                    DispatchRetirement::ProvenCancelled;
            }
            durable = retireRetryCacheDispatch(
                operationId, retirement, &storeError);
        }
        if (!durable) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheWriteFailed"), QString(),
                storeError.isEmpty()
                    ? tr("The USB outcome could not be committed to the retry store")
                    : tr("The USB outcome could not be committed to the retry store: %1")
                          .arg(storeError));
            return;
        }
        found = operations_.find(operationId);
        if (retryableOutcome) {
            if (found != operations_.end()) {
                found->info.terminalOutcome = terminalOutcome;
                found->retryMustUseNewRemoteName =
                    !finalizationUnknown;
                found->uploadFinalizationReconciliationPending =
                    finalizationUnknown;
            }
            const QString terminalMessage = finalizationUnknown
                ? tr("%1 The upload will only be reconciled through a read-only FileList check.")
                      .arg(message)
                : tr("%1 Power-cycle the printer-class device before Retry or another media action; the current firmware transfer session cannot be reused safely.")
                      .arg(message);
            if (finalizationUnknown) {
                pauseOperationForRetryCacheReconciliation(
                    operationId, terminalOutcome,
                    terminalMessage);
                startRetryCacheReadOnlyReconciliationIfReady();
            } else {
                finishOperation(
                    operationId,
                    QStringLiteral("RetryAvailable"),
                    terminalOutcome,
                    QStringLiteral("PreparedMedia"),
                    terminalMessage);
                requirePrinterRecovery(terminalMessage);
            }
            return;
        }

        removePreparedFileForOperation(operationId);
        const bool cancelled =
            outcome == PrinterProtocol::MutationOutcome::Cancelled;
        finishOperation(
            operationId,
            cancelled ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            cancelled ? QStringLiteral("UserCancelled")
                      : terminalOutcome,
            QString(), message);
        return;
    }

    const bool preservedRestrictedRecord =
        (retryCacheSnapshot_.retryCandidate.has_value() &&
         retryCacheSnapshot_.retryCandidate->operationId == operationId &&
         retryCacheSnapshot_.retryCandidate
             ->finalizationOnlyReconciliation) ||
        (retryCacheSnapshot_.inFlightDispatch.has_value() &&
         retryCacheSnapshot_.inFlightDispatch->operationId == operationId &&
         retryCacheDispatchPhaseIsRestricted(
             retryCacheSnapshot_.inFlightDispatch->phase));
    if (preservedRestrictedRecord) {
        const bool fencedDispatch =
            retryCacheSnapshot_.inFlightDispatch.has_value() &&
            retryCacheSnapshot_.inFlightDispatch->operationId ==
                operationId &&
            retryCacheDispatchPhaseIsRestricted(
                retryCacheSnapshot_.inFlightDispatch->phase);
        pauseOperationForRetryCacheReconciliation(
            operationId, terminalOutcome, message);
        if (fencedDispatch) {
            requirePrinterRecovery(tr(
                "Read-only FileList reconciliation failed. Physically reconnect the same PASE before device mutations resume."));
        }
        return;
    }

    removePreparedFileForOperation(operationId);
    const bool cancelled =
        outcome == PrinterProtocol::MutationOutcome::Cancelled ||
        found->cancelRequested;
    finishOperation(
        operationId,
        cancelled ? QStringLiteral("Cancelled")
                  : QStringLiteral("Failed"),
        cancelled ? QStringLiteral("UserCancelled")
                  : terminalOutcome,
        QString(), message);
}

bool DeviceManager::releasePrinterPreparationPath(
    const QString &path) {
    if (path.isEmpty()) {
        return true;
    }

    const QString canonicalDirectory = retryCacheStore_
        ? QDir::cleanPath(retryCacheStore_->canonicalDirectory())
        : QString();
    const QString absolutePath = QDir::cleanPath(
        QFileInfo(path).absoluteFilePath());
    if (!canonicalDirectory.isEmpty() &&
        (absolutePath == canonicalDirectory ||
         absolutePath.startsWith(
             canonicalDirectory + QDir::separator()))) {
        const QFileInfo storeArtifact(path);
        if (storeArtifact.exists() ||
            storeArtifact.isSymLink()) {
            qWarning().noquote()
                << QStringLiteral(
                       "Refusing to release RetryCacheStore-owned artifact: %1")
                       .arg(path);
            return false;
        }
        return true;
    }

    const bool removed = QFile::remove(path);
    const QFileInfo remaining(path);
    if (!removed &&
        (remaining.exists() || remaining.isSymLink())) {
        qWarning().noquote()
            << QStringLiteral(
                   "Cannot remove prepared artifact; retaining cleanup ownership: %1")
                   .arg(path);
        return false;
    }
    emit requestReleasePrinterPreparation(path);
    return true;
}

void DeviceManager::removePreparedFileForOperation(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    const QString path = found->preparedPath;
    const QString thumbnailPath = found->stagedThumbnailPath;
    releasePrinterPreparationPath(path);
    releasePrinterPreparationPath(thumbnailPath);
    found->preparedPath.clear();
    found->preparedSha256.clear();
    found->stagedThumbnailPath.clear();
    found->stagedThumbnailSha256.clear();
}

QString DeviceManager::retryCacheDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!retryCacheDirectoryOverride_.isEmpty()) {
        return retryCacheDirectoryOverride_;
    }
#endif
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("prepared-media"));
}

QString DeviceManager::deleteIntentPath() const {
    return QDir(mediaCatalogDirectory())
        .filePath(QStringLiteral("delete-intent.json"));
}

QString DeviceManager::replaceIntentPath() const {
    return QDir(mediaCatalogDirectory())
        .filePath(QStringLiteral("replace-intent.json"));
}

bool DeviceManager::writeReplaceJournal(
    const QString &operationId, const QString &stage,
    QString *errorMessage) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        !found->replaceOperation ||
        !found->replaceJournalActive ||
        found->printerProductId == 0 ||
        found->replaceJournal.productId !=
            found->printerProductId) {
        if (errorMessage) {
            *errorMessage =
                tr("Replace journal metadata or product identity is unavailable");
        }
        return false;
    }
    found->replaceJournal.formatVersion =
        TryxReplaceJournal::FormatVersion;
    found->replaceJournal.stage = stage;
    TryxReplaceJournal journal(replaceIntentPath());
    if (!journal.write(found->replaceJournal, errorMessage)) {
        pendingReplaceJournalOperationId_ = operationId;
        return false;
    }
    pendingReplaceJournalOperationId_ = operationId;
    return true;
}

bool DeviceManager::clearReplaceJournal(QString *errorMessage) {
    TryxReplaceJournal journal(replaceIntentPath());
    if (!journal.clear(errorMessage)) {
        return false;
    }
    const QString operationId =
        pendingReplaceJournalOperationId_;
    pendingReplaceJournalOperationId_.clear();
    if (!operationId.isEmpty()) {
        auto found = operations_.find(operationId);
        if (found != operations_.end()) {
            found->replaceJournalActive = false;
        }
    }
    return true;
}

void DeviceManager::loadReplaceJournal() {
    TryxReplaceJournal journal(replaceIntentPath());
    const TryxReplaceJournalLoadResult loaded = journal.load();
    if (loaded.status == TryxReplaceJournalLoadStatus::Missing) {
        pendingReplaceJournalOperationId_.clear();
        return;
    }
    if (loaded.status == TryxReplaceJournalLoadStatus::Invalid) {
        pendingReplaceJournalOperationId_ =
            QStringLiteral("invalid-replace-intent");
        qWarning().noquote()
            << "Invalid replace journal; replacements remain blocked:"
            << loaded.error;
        return;
    }
    if (loaded.record.stage == QStringLiteral("Terminal")) {
        QString clearError;
        if (!journal.clear(&clearError)) {
            pendingReplaceJournalOperationId_ =
                loaded.record.operationId;
            qWarning().noquote()
                << "Cannot clear terminal replace journal:"
                << clearError;
        }
        return;
    }

    const bool mutationOutcomeUnknown =
        (loaded.record.applyMayHaveStarted &&
         !loaded.record.applyVerified) ||
        loaded.record.fileRemoveMayHaveStarted;
    if (!mutationOutcomeUnknown) {
        TryxReplaceJournalRecord terminal =
            loaded.record;
        terminal.formatVersion =
            TryxReplaceJournal::FormatVersion;
        terminal.stage = QStringLiteral("Terminal");
        terminal.disposition =
            terminal.uploadVerified
                ? QStringLiteral("NewCopyReady")
                : QStringLiteral("OriginalRetained");
        QString recoveryError;
        if (journal.write(terminal, &recoveryError) &&
            journal.clear(&recoveryError)) {
            pendingReplaceJournalOperationId_.clear();
            OperationRecord recovered;
            recovered.info.id = terminal.operationId;
            recovered.info.kind =
                QStringLiteral("ReplaceDeviceMedia");
            recovered.info.state = terminal.uploadVerified
                ? QStringLiteral("Succeeded")
                : QStringLiteral("Failed");
            recovered.info.stage = recovered.info.state;
            recovered.info.subject =
                terminal.originalRemoteName;
            recovered.info.resultName =
                terminal.newRemoteName;
            recovered.info.deviceGeneration =
                terminal.deviceGeneration;
            recovered.printerProductId =
                terminal.productId;
            recovered.info.terminalOutcome =
                terminal.disposition;
            recovered.info.errorCategory =
                terminal.uploadVerified
                    ? QStringLiteral("OriginalRetained")
                    : QStringLiteral(
                          "InterruptedBeforeVerification");
            recovered.info.message =
                terminal.uploadVerified
                    ? tr("A replacement upload was verified before restart. The new copy is ready; Apply and Delete were not resumed.")
                    : tr("A replacement stopped before upload was verified. The original media was retained.");
            operations_.insert(recovered.info.id,
                               recovered);
            operationOrder_.append(recovered.info.id);
            publishOperation(recovered.info.id);
            return;
        }
        qWarning().noquote()
            << "Cannot settle safe replace journal after restart:"
            << recoveryError;
    }

    pendingReplaceJournalOperationId_ =
        loaded.record.operationId;
    OperationRecord record;
    if (operations_.contains(loaded.record.operationId)) {
        record = operations_.value(loaded.record.operationId);
    }
    record.info.id = loaded.record.operationId;
    record.info.kind = QStringLiteral("ReplaceDeviceMedia");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("ReconcileOnly");
    record.info.subject = loaded.record.originalRemoteName;
    record.info.resultName = loaded.record.newRemoteName;
    record.info.deviceGeneration = loaded.record.deviceGeneration;
    record.info.retryMode = QStringLiteral("ReconcileOnly");
    record.info.terminalOutcome =
        loaded.record.disposition;
    record.info.errorCategory = mutationOutcomeUnknown
        ? QStringLiteral("PartialOrUnknown")
        : loaded.record.uploadVerified
            ? QStringLiteral("NewCopyReady")
            : QStringLiteral("OriginalRetained");
    record.info.message = mutationOutcomeUnknown
        ? tr("A previous replacement stopped after a mutation may have started. Apply and Delete will not be repeated automatically.")
        : loaded.record.uploadVerified
            ? tr("A replacement upload was verified before restart. The new copy is ready; Apply and Delete were not resumed.")
            : tr("A replacement stopped before upload was verified. The original media was retained.");
    record.printerProductId = loaded.record.productId;
    record.originalMediaId =
        loaded.record.originalMediaId;
    record.originalRemoteNameForReplace =
        loaded.record.originalRemoteName;
    record.artifactId = loaded.record.artifactId;
    record.sourceContentSha256 =
        loaded.record.decodedSha256;
    record.uploadDeviceIdentity =
        loaded.record.deviceIdentity;
    record.uploadDeviceGeneration =
        loaded.record.deviceGeneration;
    record.remoteName = loaded.record.newRemoteName;
    record.replaceOperation = true;
    record.replaceJournalActive = true;
    record.replaceJournal = loaded.record;
    if (!operations_.contains(record.info.id)) {
        operations_.insert(record.info.id, record);
        operationOrder_.append(record.info.id);
    } else {
        operations_[record.info.id] = record;
    }
    publishOperation(record.info.id);
}

void DeviceManager::resumePendingReplaceReconciliation() {
    if (firmwareExclusiveActive() ||
        pendingReplaceJournalOperationId_.isEmpty() ||
        !pendingDeleteOperationId_.isEmpty() ||
        !activeOperationId_.isEmpty() ||
        !printerDisplaySessionActive_ ||
        currentPrinterPath().isEmpty() ||
        !operations_.contains(
            pendingReplaceJournalOperationId_)) {
        return;
    }
    OperationRecord &record =
        operations_[pendingReplaceJournalOperationId_];
    if (!record.replaceOperation ||
        !record.replaceJournalActive ||
        record.replaceJournal.productId !=
            record.printerProductId ||
        !operationMatchesCurrentPrinterProduct(record) ||
        record.replaceJournal.deviceIdentity !=
            printerDeviceSerial_.trimmed() ||
        !PrinterProtocol::isSafeUploadMediaName(
            record.originalRemoteNameForReplace)) {
        return;
    }
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    if (record.replaceJournal.fileRemoveMayHaveStarted) {
        record.info.state = QStringLiteral("Refreshing");
        record.info.stage =
            QStringLiteral("ReconcilingUnknownDelete");
        record.info.retryMode.clear();
        record.info.message = tr(
            "Re-reading FileList without repeating FileRemove...");
        record.info.deviceGeneration = printerGeneration_;
        record.deleteNames = {
            record.originalRemoteNameForReplace};
        record.deleteReconcileOnly = true;
        activeOperationId_ = record.info.id;
        publishOperation(record.info.id);
        emit requestBeginPrinterForegroundOperation(
            record.info.id, printerGeneration_);
        emit requestPrinterDeleteMedia(
            currentPrinterPath(), record.deleteNames,
            record.info.id, deleteIntentPath(), true,
            static_cast<qint64>(
                record.replaceJournal.originalSize),
            record.replaceJournal.newRemoteName,
            static_cast<qint64>(
                record.replaceJournal.newSize),
            printerGeneration_);
        return;
    }
    if (!record.replaceJournal.applyMayHaveStarted ||
        record.replaceJournal.applyVerified) {
        return;
    }
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage =
        QStringLiteral("ReconcilingUnknownApply");
    record.info.retryMode.clear();
    record.info.message = tr(
        "Re-reading media references without repeating Apply...");
    record.info.deviceGeneration = printerGeneration_;
    activeOperationId_ = record.info.id;
    publishOperation(record.info.id);
    emit requestBeginPrinterForegroundOperation(
        record.info.id, printerGeneration_);
    emit requestPrinterReplacePreflight(
        currentPrinterPath(),
        record.originalRemoteNameForReplace,
        static_cast<qint64>(
            record.replaceJournal.originalSize),
        record.replaceJournal.newRemoteName,
        static_cast<qint64>(
            record.replaceJournal.newSize),
        record.info.id, printerGeneration_);
}

bool DeviceManager::writeDeleteIntent(
    const QString &operationId, const QString &stage,
    bool mayHaveStarted, int currentIndex,
    const QString &currentName, const QStringList &deletedNames,
    QString *errorMessage) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() ||
        found->deleteNames.isEmpty() || currentIndex < 0 ||
        currentIndex >= found->deleteNames.size() ||
        found->deleteNames.at(currentIndex) != currentName ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Delete intent metadata is incomplete");
        }
        return false;
    }
    const auto catalogEntry = std::find_if(
        mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
        [&currentName](const TryxRuntimeMediaEntry &entry) {
            return entry.name == currentName;
        });
    if (catalogEntry == mediaCatalog_.entries.cend()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Delete target is absent from the typed media catalog");
        }
        return false;
    }
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto existing = store.load();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    tryx::DeleteIntentRecord intent;
    intent.operationId = operationId;
    intent.productId = found->printerProductId;
    intent.deviceIdentity = printerDeviceSerial_.trimmed();
    intent.deviceGeneration = found->info.deviceGeneration;
    intent.requestedNames = found->deleteNames;
    intent.deletedNames = deletedNames;
    intent.currentIndex = currentIndex;
    intent.currentName = currentName;
    intent.currentSize = catalogEntry->size;
    intent.currentSource = catalogEntry->source;
    intent.currentReadOnly = catalogEntry->readOnly;
    intent.stage = stage;
    intent.mayHaveStarted = mayHaveStarted;
    intent.createdUtc = existing.loaded() &&
                            existing.record.operationId == operationId
        ? existing.record.createdUtc
        : now;
    intent.updatedUtc = now;
    const auto persisted = store.write(intent);
    if (!persisted.ok()) {
        if (errorMessage) {
            *errorMessage = persisted.detail;
        }
        return false;
    }
    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = operationId;
    return true;
}

bool DeviceManager::clearDeleteIntent(
    const QString &expectedOperationId, QString *errorMessage) {
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto loaded = store.load();
    if (loaded.status ==
        tryx::DeleteIntentStore::LoadStatus::Missing) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_.clear();
        return true;
    }
    const QString expectedDeviceIdentity =
        pendingDeleteIntent_.has_value() &&
                pendingDeleteIntent_->operationId == expectedOperationId
            ? pendingDeleteIntent_->deviceIdentity
            : QString();
    if (expectedDeviceIdentity.isEmpty()) {
        if (errorMessage) {
            *errorMessage = loaded.detail.isEmpty()
                ? tr("Delete intent identity is unavailable")
                : loaded.detail;
        }
        return false;
    }
    const auto cleared = store.clear(expectedOperationId,
                                     expectedDeviceIdentity);
    if (!cleared.ok()) {
        if (errorMessage) {
            *errorMessage = cleared.detail;
        }
        return false;
    }
    pendingDeleteIntent_.reset();
    pendingDeleteOperationId_.clear();
    return true;
}

void DeviceManager::loadDeleteIntent() {
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto loaded = store.load();
    if (loaded.status ==
        tryx::DeleteIntentStore::LoadStatus::Missing) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_.clear();
        return;
    }
    if (!loaded.loaded()) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_ = QStringLiteral("invalid-delete-intent");
        qWarning().noquote()
            << "Delete intent was not accepted; deletes remain blocked:"
            << loaded.detail;
        return;
    }
    const tryx::DeleteIntentRecord &intent = loaded.record;
    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = intent.operationId;
    if (!intent.mayHaveStarted) {
        QString cleanupError;
        if (!clearDeleteIntent(intent.operationId, &cleanupError)) {
            qWarning().noquote() << cleanupError;
        }
        return;
    }

    OperationRecord record;
    if (operations_.contains(intent.operationId)) {
        record = operations_.value(intent.operationId);
    }
    record.info.id = intent.operationId;
    record.info.kind = record.replaceOperation
        ? QStringLiteral("ReplaceDeviceMedia")
        : QStringLiteral("DeleteMedia");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("RetryAvailable");
    record.info.errorCategory = QStringLiteral("PartialOrUnknown");
    record.info.retryMode = QStringLiteral("DeleteReconcile");
    record.info.subject = intent.currentName;
    record.info.resultName = intent.currentName;
    record.info.deviceGeneration = intent.deviceGeneration;
    record.info.message = tr(
        "A previous delete command requires read-only FileList reconciliation");
    if (!record.replaceOperation || !record.replaceJournalActive) {
        record.printerProductId = intent.productId;
    }
    record.deleteNames = intent.requestedNames;
    record.deletedNames = intent.deletedNames;
    record.deleteReconcileOnly = true;
    if (!operations_.contains(intent.operationId)) {
        operations_.insert(intent.operationId, record);
        operationOrder_.append(intent.operationId);
    } else {
        operations_[intent.operationId] = record;
    }
    publishOperation(intent.operationId);
}

void DeviceManager::resumePendingDeleteReconciliation() {
    if (firmwareExclusiveActive() ||
        pendingDeleteOperationId_.isEmpty() ||
        retryCacheMutationGateActive() ||
        !activeOperationId_.isEmpty() ||
        !printerDisplaySessionActive_ ||
        currentPrinterPath().isEmpty() ||
        !pendingDeleteIntent_.has_value() ||
        pendingDeleteIntent_->operationId !=
            pendingDeleteOperationId_ ||
        !operations_.contains(pendingDeleteOperationId_)) {
        return;
    }
    const tryx::DeleteIntentRecord &intent =
        *pendingDeleteIntent_;
    OperationRecord &record =
        operations_[pendingDeleteOperationId_];
    if (record.info.id != pendingDeleteOperationId_ ||
        record.printerProductId != intent.productId) {
        return;
    }

    const QString expectedIdentity =
        intent.deviceIdentity.trimmed();
    if (expectedIdentity.isEmpty() ||
        expectedIdentity != printerDeviceSerial_.trimmed()) {
        return;
    }
    const QString currentName = intent.currentName;
    if (!PrinterProtocol::isSafeUploadMediaName(currentName)) {
        return;
    }
    const bool hasReplaceState =
        record.replaceOperation ||
        !pendingReplaceJournalOperationId_.isEmpty();
    if (hasReplaceState &&
        (pendingReplaceJournalOperationId_ !=
             pendingDeleteOperationId_ ||
         !record.replaceOperation ||
         !record.replaceJournalActive ||
         record.replaceJournal.operationId !=
             pendingDeleteOperationId_ ||
         record.replaceJournal.productId != intent.productId ||
         record.replaceJournal.deviceIdentity.trimmed() !=
             expectedIdentity ||
         record.uploadDeviceIdentity.trimmed() !=
             expectedIdentity ||
         record.replaceJournal.deviceGeneration !=
             intent.deviceGeneration ||
         record.uploadDeviceGeneration != intent.deviceGeneration ||
         record.originalRemoteNameForReplace != currentName ||
         record.replaceJournal.originalRemoteName != currentName ||
         record.replaceJournal.originalSize != intent.currentSize)) {
        return;
    }
    if (!operationMatchesCurrentPrinterProduct(record)) {
        return;
    }
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("ReconcilingDelete");
    record.info.retryMode.clear();
    record.info.message = tr(
        "Reconciling the previous delete command without repeating it...");
    record.info.deviceGeneration = printerGeneration_;
    record.deleteReconcileOnly = true;
    activeOperationId_ = record.info.id;
    publishOperation(record.info.id);
    emit requestBeginPrinterForegroundOperation(record.info.id,
                                                 printerGeneration_);
    emit requestPrinterDeleteMedia(
        currentPrinterPath(), QStringList{currentName}, record.info.id,
        deleteIntentPath(), true,
        record.replaceOperation
            ? static_cast<qint64>(
                  record.replaceJournal.originalSize)
            : 0,
        record.replaceOperation
            ? record.replaceJournal.newRemoteName
            : QString(),
        record.replaceOperation
            ? static_cast<qint64>(
                  record.replaceJournal.newSize)
            : 0,
        printerGeneration_);
}

tryx::RetryCacheStore &DeviceManager::retryCacheStore() {
    const QString directory = retryCacheDirectory();
    if (!retryCacheStore_ ||
        retryCacheStore_->canonicalDirectory() !=
            QDir(directory).filePath(QStringLiteral("v11"))) {
        retryCacheStore_ =
            std::make_unique<tryx::RetryCacheStore>(directory);
        retryCacheSnapshot_ = {};
        retryCacheLoadComplete_ = false;
        retryCacheStartupFailure_ = false;
        retryCacheFailureDetail_.clear();
        pendingRetryCacheValidations_.clear();
    }
    return *retryCacheStore_;
}

QString DeviceManager::retryCacheArtifactPath(
    const tryx::RetryCacheStore::StoredArtifact &artifact) const {
    if (!retryCacheStore_ || artifact.name.isEmpty() ||
        QFileInfo(artifact.name).fileName() != artifact.name) {
        return {};
    }
    return QDir(retryCacheStore_->canonicalDirectory())
        .filePath(artifact.name);
}

tryx::RetryCacheStore::ExpectedDispatch
DeviceManager::retryCacheExpectedDispatch(
    const tryx::RetryCacheStore::StoredRetryCandidate &candidate) const {
    tryx::RetryCacheStore::ExpectedDispatch expected;
    expected.lineageId = candidate.lineageId;
    expected.dispatchId = candidate.dispatchId;
    expected.operationId = candidate.operationId;
    expected.productId = candidate.productId;
    expected.deviceIdentity = candidate.deviceIdentity;
    expected.deviceGeneration = candidate.deviceGeneration;
    return expected;
}

tryx::RetryCacheStore::ExpectedDispatch
DeviceManager::retryCacheExpectedDispatch(
    const tryx::RetryCacheStore::StoredDispatch &dispatch) const {
    tryx::RetryCacheStore::ExpectedDispatch expected;
    expected.lineageId = dispatch.lineageId;
    expected.dispatchId = dispatch.dispatchId;
    expected.operationId = dispatch.operationId;
    expected.productId = dispatch.productId;
    expected.deviceIdentity = dispatch.deviceIdentity;
    expected.deviceGeneration = dispatch.deviceGeneration;
    return expected;
}

DeviceManager::OperationRecord DeviceManager::retryCacheOperationRecord(
    const tryx::RetryCacheStore::StoredRetryCandidate &candidate) const {
    OperationRecord record;
    record.info.id = candidate.operationId;
    record.info.kind = candidate.attempt > 1
        ? QStringLiteral("UploadRetry")
        : QStringLiteral("Upload");
    record.info.state = candidate.finalizationOnlyReconciliation
        ? QStringLiteral("Refreshing")
        : QStringLiteral("RetryAvailable");
    record.info.stage = candidate.finalizationOnlyReconciliation
        ? QStringLiteral("RecoveringFinalization")
        : QStringLiteral("RetryAvailable");
    record.info.errorCategory =
        retryCacheTerminalOutcomeName(candidate.outcome);
    record.info.terminalOutcome =
        retryCacheTerminalOutcomeName(candidate.outcome);
    record.info.primaryErrorCategory =
        candidate.primaryErrorCategory;
    record.info.primaryErrorMessage =
        candidate.primaryErrorMessage;
    record.info.retryMode = candidate.finalizationOnlyReconciliation
        ? QString()
        : QStringLiteral("PreparedMedia");
    record.info.subject = candidate.subject;
    record.info.resultName = candidate.retryRemoteName;
    record.info.completed = candidate.confirmedBytes;
    record.info.total = candidate.prepared.size;
    record.info.confirmedBytes = candidate.confirmedBytes;
    record.info.lastConfirmedChunkIndex =
        candidate.lastConfirmedChunkIndex;
    record.info.attempt = candidate.attempt;
    record.info.deviceGeneration = printerGeneration_;
    record.preparedPath = retryCacheArtifactPath(candidate.prepared);
    record.preparedSha256 = candidate.prepared.sha256;
    if (candidate.thumbnail.has_value()) {
        record.stagedThumbnailPath =
            retryCacheArtifactPath(*candidate.thumbnail);
        record.stagedThumbnailSha256 =
            candidate.thumbnail->sha256;
    }
    if (candidate.origin.has_value()) {
        record.sourceContentSha256 =
            candidate.origin->sourceContentSha256;
        record.sourceSize =
            candidate.origin->sourceContentSize;
        record.conversionProfile =
            candidate.origin->conversionProfile;
    }
    record.printerProductId = candidate.productId;
    record.mediaConversion = candidate.conversion;
    if (const auto profile = printerProductProfileForId(
            candidate.productId)) {
        const QSize size =
            tryx::printer_media_identity::
                printerMediaSizeForConversionIdentity(
                    candidate.conversion, *profile);
        if (size.isValid() && size.width() != profile->mediaWidth) {
            record.mediaPreparationProfile.target =
                QStringLiteral("SplitArea");
        }
    }
    record.mediaTransform = record.mediaPreparationProfile.transform;
    record.remoteName = candidate.retryRemoteName;
    record.originalRemoteName = candidate.originalRemoteName;
    record.uploadDeviceIdentity = candidate.deviceIdentity;
    record.uploadDeviceGeneration = candidate.deviceGeneration;
    record.requiresDeviceRecovery =
        candidate.requiresDeviceRecovery;
    record.retryMustUseNewRemoteName =
        candidate.requiresNewRemoteName;
    record.uploadFinalizationReconciliationPending =
        candidate.finalizationOnlyReconciliation;
    record.retryLineageId = candidate.lineageId;
    record.retryDispatchId = candidate.dispatchId;
    if (candidate.finalizationOnlyReconciliation) {
        record.info.message = tr(
            "The completed upload is restricted to read-only FileList reconciliation; media data will not be retransmitted.");
    } else if (candidate.requiresDeviceRecovery) {
        record.info.message = tr(
            "The previous PASE transfer requires a physical reconnect of the same device before Retry.");
    } else if (candidate.requiresNewRemoteName) {
        record.info.message = tr(
            "Prepared media is available for a new transfer under a new device filename.");
    } else {
        record.info.message =
            tr("A verified prepared upload is available for manual retry");
    }
    return record;
}

DeviceManager::OperationRecord DeviceManager::retryCacheOperationRecord(
    const tryx::RetryCacheStore::StoredDispatch &dispatch) const {
    OperationRecord record;
    record.info.id = dispatch.operationId;
    record.info.kind = dispatch.attempt > 1
        ? QStringLiteral("UploadRetry")
        : QStringLiteral("Upload");
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RecoveringFinalization");
    record.info.errorCategory = QStringLiteral("ShadowMissingFence");
    record.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
    record.info.primaryErrorCategory =
        dispatch.primaryErrorCategory;
    record.info.primaryErrorMessage =
        dispatch.primaryErrorMessage;
    record.info.subject = dispatch.subject;
    record.info.resultName = dispatch.retryRemoteName;
    record.info.completed = dispatch.confirmedBytes;
    record.info.total = dispatch.prepared.size;
    record.info.confirmedBytes = dispatch.confirmedBytes;
    record.info.lastConfirmedChunkIndex =
        dispatch.lastConfirmedChunkIndex;
    record.info.attempt = dispatch.attempt;
    record.info.deviceGeneration = printerGeneration_;
    record.info.message = tr(
        "A downgrade removed the expected retry shadow. Only read-only FileList reconciliation or a physical reconnect is allowed.");
    record.preparedPath = retryCacheArtifactPath(dispatch.prepared);
    record.preparedSha256 = dispatch.prepared.sha256;
    if (dispatch.thumbnail.has_value()) {
        record.stagedThumbnailPath =
            retryCacheArtifactPath(*dispatch.thumbnail);
        record.stagedThumbnailSha256 =
            dispatch.thumbnail->sha256;
    }
    if (dispatch.origin.has_value()) {
        record.sourceContentSha256 =
            dispatch.origin->sourceContentSha256;
        record.sourceSize =
            dispatch.origin->sourceContentSize;
        record.conversionProfile =
            dispatch.origin->conversionProfile;
    }
    record.printerProductId = dispatch.productId;
    record.mediaConversion = dispatch.conversion;
    if (const auto profile = printerProductProfileForId(
            dispatch.productId)) {
        const QSize size =
            tryx::printer_media_identity::
                printerMediaSizeForConversionIdentity(
                    dispatch.conversion, *profile);
        if (size.isValid() && size.width() != profile->mediaWidth) {
            record.mediaPreparationProfile.target =
                QStringLiteral("SplitArea");
        }
    }
    record.mediaTransform = record.mediaPreparationProfile.transform;
    record.remoteName = dispatch.retryRemoteName;
    record.originalRemoteName = dispatch.originalRemoteName;
    record.uploadDeviceIdentity = dispatch.deviceIdentity;
    record.uploadDeviceGeneration = dispatch.deviceGeneration;
    record.requiresDeviceRecovery = dispatch.requiresDeviceRecovery;
    record.retryMustUseNewRemoteName = true;
    record.uploadFinalizationReconciliationPending = true;
    record.retryLineageId = dispatch.lineageId;
    record.retryDispatchId = dispatch.dispatchId;
    return record;
}

QString DeviceManager::retryCacheVisibleOperationId() const {
    if (retryCacheSnapshot_.inFlightDispatch.has_value()) {
        return retryCacheSnapshot_.inFlightDispatch->operationId;
    }
    if (retryCacheSnapshot_.retryCandidate.has_value()) {
        return retryCacheSnapshot_.retryCandidate->operationId;
    }
    return {};
}

bool DeviceManager::retryCacheStoreBlocksMutations() const {
    if (!retryCacheStartupFailure_ &&
        (!pendingRetryCacheValidations_.isEmpty() ||
         !retryCacheLoadComplete_)) {
        return false;
    }
    return retryCacheStartupFailure_ ||
        (retryCacheStore_ && retryCacheStore_->blocksMutations());
}

bool DeviceManager::retryCacheStartupSessionGateActive() const {
    return !retryCacheLoadComplete_ ||
        !pendingRetryCacheValidations_.isEmpty() ||
        retryCacheStoreBlocksMutations();
}

bool DeviceManager::retryCacheRestrictedRecoveryActive() const {
    if (retryCacheSnapshot_.retryCandidate.has_value() &&
        retryCacheSnapshot_.retryCandidate
            ->finalizationOnlyReconciliation) {
        return true;
    }
    return retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheDispatchPhaseIsRestricted(
            retryCacheSnapshot_.inFlightDispatch->phase);
}

bool DeviceManager::retryCacheMutationGateActive() const {
    return runtimeDowngradeV10Prepared_ ||
        retryCacheStartupSessionGateActive() ||
        retryCacheRestrictedRecoveryActive();
}

void DeviceManager::synchronizeRetryCacheSurface() {
    const QString visibleOperationId = retryCacheVisibleOperationId();
    const QStringList ids = operationOrder_;
    for (const QString &operationId : ids) {
        if (operationId == visibleOperationId ||
            operationId == activeOperationId_) {
            continue;
        }
        const auto found = operations_.constFind(operationId);
        if (found == operations_.constEnd() ||
            found->retryLineageId.isEmpty() ||
            (found->info.state != QStringLiteral("RetryAvailable") &&
             !found->uploadFinalizationReconciliationPending)) {
            continue;
        }
        operations_.remove(operationId);
        operationOrder_.removeAll(operationId);
    }

    if (visibleOperationId.isEmpty()) {
        return;
    }

    if (retryCacheSnapshot_.inFlightDispatch.has_value()) {
        const auto &dispatch =
            *retryCacheSnapshot_.inFlightDispatch;
        auto existing = operations_.find(visibleOperationId);
        if (existing != operations_.end() &&
            (dispatch.phase ==
                 tryx::RetryCacheStore::DispatchPhase::Preparing ||
             dispatch.phase ==
                 tryx::RetryCacheStore::DispatchPhase::DispatchArmed)) {
            existing->retryLineageId = dispatch.lineageId;
            existing->retryDispatchId = dispatch.dispatchId;
            existing->preparedPath =
                retryCacheArtifactPath(dispatch.prepared);
            existing->preparedSha256 = dispatch.prepared.sha256;
            existing->stagedThumbnailPath =
                dispatch.thumbnail.has_value()
                ? retryCacheArtifactPath(*dispatch.thumbnail)
                : QString();
            existing->stagedThumbnailSha256 =
                dispatch.thumbnail.has_value()
                ? dispatch.thumbnail->sha256
                : QString();
            return;
        }
    }

    if (retryCacheSnapshot_.retryCandidate.has_value()) {
        const auto &candidate =
            *retryCacheSnapshot_.retryCandidate;
        auto existing = operations_.find(visibleOperationId);
        if (existing != operations_.end() &&
            activeOperationId_ == visibleOperationId &&
            existing->retryLineageId == candidate.lineageId &&
            existing->retryDispatchId == candidate.dispatchId) {
            existing->preparedPath =
                retryCacheArtifactPath(candidate.prepared);
            existing->preparedSha256 = candidate.prepared.sha256;
            existing->stagedThumbnailPath =
                candidate.thumbnail.has_value()
                ? retryCacheArtifactPath(*candidate.thumbnail)
                : QString();
            existing->stagedThumbnailSha256 =
                candidate.thumbnail.has_value()
                ? candidate.thumbnail->sha256
                : QString();
            existing->info.total = candidate.prepared.size;
            existing->requiresDeviceRecovery =
                candidate.requiresDeviceRecovery;
            existing->retryMustUseNewRemoteName =
                candidate.requiresNewRemoteName;
            existing->uploadFinalizationReconciliationPending =
                candidate.finalizationOnlyReconciliation;
            return;
        }
    }

    OperationRecord restored =
        retryCacheSnapshot_.inFlightDispatch.has_value()
        ? retryCacheOperationRecord(
              *retryCacheSnapshot_.inFlightDispatch)
        : retryCacheOperationRecord(
              *retryCacheSnapshot_.retryCandidate);
    const auto existing = operations_.constFind(visibleOperationId);
    if (existing != operations_.constEnd()) {
        restored.info.parentId = existing->info.parentId;
    }
    operations_.insert(visibleOperationId, restored);
    if (!operationOrder_.contains(visibleOperationId)) {
        operationOrder_.append(visibleOperationId);
    }
    publishOperation(visibleOperationId);
}

void DeviceManager::queueRetryCacheValidationRequests(
    const QVector<tryx::RetryCacheStore::ValidationRequest> &requests) {
    if (requests.isEmpty()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = tr(
            "Retry-cache validation was requested without any artifacts");
        qWarning().noquote() << retryCacheFailureDetail_;
        return;
    }
    pendingRetryCacheValidations_.clear();
    for (const auto &request : requests) {
        if (request.token.isEmpty() ||
            pendingRetryCacheValidations_.contains(request.token)) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tr(
                "Retry-cache validation returned an invalid token set");
            pendingRetryCacheValidations_.clear();
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        pendingRetryCacheValidations_.insert(request.token, request);
    }
    for (const auto &request : requests) {
        emit requestValidatePrinterRetryCacheArtifact(
            request.token, request.path, request.expectedSize,
            request.expectedSha256, request.expectedDevice,
            request.expectedInode);
    }
}

bool DeviceManager::adoptLoadedRetryCacheSnapshot(
    const tryx::RetryCacheStore::Snapshot &loadedSnapshot,
    QString *errorMessage) {
    tryx::RetryCacheStore::Snapshot snapshot = loadedSnapshot;
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::
                LocalCommitPending) {
        const auto dispatch = *snapshot.inFlightDispatch;
        const auto deferred = retryCacheStore().deferLocalCommit(
            snapshot, retryCacheExpectedDispatch(dispatch),
            dispatch.primaryErrorCategory.isEmpty()
                ? QStringLiteral("LocalMediaCommitInterrupted")
                : dispatch.primaryErrorCategory,
            dispatch.primaryErrorMessage.isEmpty()
                ? tr("The remote upload was verified before restart, but its local catalog commit must be retried")
                : dispatch.primaryErrorMessage);
        if (!deferred.ok() || !deferred.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = deferred.detail.isEmpty()
                ? tr("Interrupted local media commit could not be folded into one retry candidate")
                : deferred.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *deferred.snapshot;
    }
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::PartialOrUnknown) {
        const auto resolved = retryCacheStore().resolveRecoveredDispatch(
            snapshot,
            retryCacheExpectedDispatch(
                *snapshot.inFlightDispatch));
        if (!resolved.ok() || !resolved.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = resolved.detail.isEmpty()
                ? tr("Recovered retry dispatch could not be folded into one candidate")
                : resolved.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *resolved.snapshot;
    }
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::
                ShadowMissingFenceReconnectPending) {
        const auto resolved = retryCacheStore()
            .resolveShadowMissingFence(
                snapshot,
                retryCacheExpectedDispatch(
                    *snapshot.inFlightDispatch),
                tryx::RetryCacheStore::RecoveryFenceProof::
                    PhysicalReconnectObserved);
        if (!resolved.ok() || !resolved.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = resolved.detail.isEmpty()
                ? tr("Interrupted physical retry-fence recovery could not be resumed")
                : resolved.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *resolved.snapshot;
    }

    if (snapshot.retryCandidate.has_value() &&
        !snapshot.inFlightDispatch.has_value()) {
        const auto &candidate = *snapshot.retryCandidate;
        const auto existing = operations_.constFind(
            candidate.operationId);
        const bool exactExisting =
            existing != operations_.constEnd() &&
            existing->retryLineageId == candidate.lineageId &&
            existing->retryDispatchId == candidate.dispatchId &&
            existing->printerProductId == candidate.productId &&
            existing->uploadDeviceIdentity ==
                candidate.deviceIdentity &&
            existing->uploadDeviceGeneration ==
                candidate.deviceGeneration;
        if (existing != operations_.constEnd() && !exactExisting) {
            QString replacementId;
            do {
                replacementId = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
            } while (operations_.contains(replacementId));
            const auto remapped =
                retryCacheStore().remapCandidateOperationId(
                    snapshot,
                    retryCacheExpectedDispatch(candidate),
                    replacementId);
            if (!remapped.ok() || !remapped.snapshot.has_value()) {
                retryCacheStartupFailure_ = true;
                retryCacheFailureDetail_ = remapped.detail.isEmpty()
                    ? tr("Stored retry operation ID could not be remapped durably")
                    : remapped.detail;
                if (errorMessage) {
                    *errorMessage = retryCacheFailureDetail_;
                }
                return false;
            }
            snapshot = *remapped.snapshot;
        }
    } else if (snapshot.inFlightDispatch.has_value()) {
        const auto &dispatch = *snapshot.inFlightDispatch;
        const auto existing =
            operations_.constFind(dispatch.operationId);
        if (existing != operations_.constEnd() &&
            (existing->retryLineageId != dispatch.lineageId ||
             existing->retryDispatchId != dispatch.dispatchId ||
             existing->printerProductId != dispatch.productId ||
             existing->uploadDeviceIdentity !=
                 dispatch.deviceIdentity ||
             existing->uploadDeviceGeneration !=
                 dispatch.deviceGeneration)) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tr(
                "A fenced retry dispatch conflicts with an existing operation ID and cannot be remapped safely");
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
    }

    retryCacheSnapshot_ = snapshot;
    retryCacheLoadComplete_ = true;
    retryCacheStartupFailure_ = false;
    retryCacheFailureDetail_.clear();
    synchronizeRetryCacheSurface();

    if (snapshot.retryCandidate.has_value() &&
        snapshot.retryCandidate->requiresDeviceRecovery &&
        !snapshot.retryCandidate
             ->finalizationOnlyReconciliation) {
        requirePrinterRecovery(tr(
            "A prepared upload was restored after an incomplete PASE transfer. Physically reconnect the same device before Retry."));
    } else if (!retryCacheRestrictedRecoveryActive()) {
        resumePrinterSessionAfterRetryCacheValidation();
    }
    startRetryCacheReadOnlyReconciliationIfReady();
    return true;
}

void DeviceManager::loadRetryCache() {
    if (!pendingRetryCacheValidations_.isEmpty()) {
        return;
    }
    retryCacheLoadComplete_ = false;
    retryCacheStartupFailure_ = false;
    retryCacheFailureDetail_.clear();

    const auto loaded = retryCacheStore().load();
    switch (loaded.status) {
    case tryx::RetryCacheStore::LoadStatus::Missing:
        retryCacheSnapshot_ = {};
        retryCacheLoadComplete_ = true;
        synchronizeRetryCacheSurface();
        resumePrinterSessionAfterRetryCacheValidation();
        return;
    case tryx::RetryCacheStore::LoadStatus::Loaded: {
        if (!loaded.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tr(
                "Retry-cache load succeeded without a snapshot");
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        QString error;
        if (!adoptLoadedRetryCacheSnapshot(
                *loaded.snapshot, &error)) {
            qWarning().noquote()
                << tr("Cannot adopt stored retry state: %1")
                       .arg(error);
        }
        return;
    }
    case tryx::RetryCacheStore::LoadStatus::NeedsValidation:
        if (!loaded.snapshot.has_value() ||
            loaded.validationRequests.isEmpty()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tr(
                "Retry-cache load returned an incomplete validation request");
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        queueRetryCacheValidationRequests(
            loaded.validationRequests);
        return;
    case tryx::RetryCacheStore::LoadStatus::UnsupportedVersion:
    case tryx::RetryCacheStore::LoadStatus::Invalid:
    case tryx::RetryCacheStore::LoadStatus::Unsafe:
    case tryx::RetryCacheStore::LoadStatus::ReadFailed:
    case tryx::RetryCacheStore::LoadStatus::ResourceLimitExceeded:
    case tryx::RetryCacheStore::LoadStatus::Conflict:
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = loaded.detail.isEmpty()
            ? tr("Retry-cache state is invalid or unsafe")
            : loaded.detail;
        qWarning().noquote()
            << tr("Retry-cache startup remains fail-closed: %1")
                   .arg(retryCacheFailureDetail_);
        return;
    }
}

void DeviceManager::handleRetryCacheArtifactValidation(
    const QString &validationToken, bool valid,
    bool cancelled, qint64 actualSize,
    const QString &actualSha256, quint64 actualDevice,
    quint64 actualInode, const QString &message) {
    const auto pending =
        pendingRetryCacheValidations_.constFind(validationToken);
    if (pending == pendingRetryCacheValidations_.constEnd()) {
        return;
    }
    printerMediaPreparer_->clearRetryValidationCancellation(
        validationToken);
    pendingRetryCacheValidations_.remove(validationToken);

    tryx::RetryCacheStore::ValidationResult validation;
    validation.token = validationToken;
    validation.valid = valid;
    validation.cancelled = cancelled;
    validation.actualSize = actualSize;
    validation.actualSha256 = actualSha256;
    validation.actualDevice = actualDevice;
    validation.actualInode = actualInode;
    validation.detail = message;
    const auto completed =
        retryCacheStore().completeValidation(validation);
    if (!completed.ok()) {
        for (auto it = pendingRetryCacheValidations_.cbegin();
             it != pendingRetryCacheValidations_.cend(); ++it) {
            printerMediaPreparer_->cancelRetryValidation(it.key());
        }
        pendingRetryCacheValidations_.clear();
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = completed.detail.isEmpty()
            ? tr("Retry-cache artifact validation failed")
            : completed.detail;
        qWarning().noquote()
            << tr("Retry-cache validation remains fail-closed: %1")
                   .arg(retryCacheFailureDetail_);
        return;
    }
    if (!completed.snapshot.has_value()) {
        if (pendingRetryCacheValidations_.isEmpty()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tr(
                "Retry-cache validation completed without a snapshot");
            qWarning().noquote() << retryCacheFailureDetail_;
        }
        return;
    }
    if (!pendingRetryCacheValidations_.isEmpty()) {
        for (auto it = pendingRetryCacheValidations_.cbegin();
             it != pendingRetryCacheValidations_.cend(); ++it) {
            printerMediaPreparer_->cancelRetryValidation(it.key());
        }
        pendingRetryCacheValidations_.clear();
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = tr(
            "Retry-cache validation produced a snapshot before all tokens completed");
        qWarning().noquote() << retryCacheFailureDetail_;
        return;
    }
    QString error;
    if (!adoptLoadedRetryCacheSnapshot(
            *completed.snapshot, &error)) {
        qWarning().noquote()
            << tr("Cannot adopt validated retry state: %1")
                   .arg(error);
    }
}

bool DeviceManager::recordRetryCacheOutcome(
    const QString &operationId,
    tryx::RetryCacheStore::TerminalOutcome outcome,
    qint64 confirmedBytes, const QString &errorCategory,
    const QString &errorMessage, QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId !=
            operationId) {
        if (storeError) {
            *storeError = tr(
                "The operation is not the current retry dispatch");
        }
        return false;
    }
    const auto dispatch =
        *retryCacheSnapshot_.inFlightDispatch;
    tryx::RetryCacheStore::RetryableOutcomeInput input;
    input.outcome = outcome;
    input.confirmedBytes = confirmedBytes;
    input.primaryErrorCategory = errorCategory;
    input.primaryErrorMessage = errorMessage;
    const auto result = retryCacheStore().recordRetryableOutcome(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(dispatch), input);
    if (!result.ok() || !result.snapshot.has_value()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("Retry outcome could not be persisted")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface();
    return true;
}

bool DeviceManager::beginRetryCacheLocalCommit(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *storeError) {
    std::optional<tryx::RetryCacheStore::ExpectedDispatch> expected;
    if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->operationId ==
            operationId) {
        expected = retryCacheExpectedDispatch(
            *retryCacheSnapshot_.inFlightDispatch);
    } else if (retryCacheSnapshot_.retryCandidate.has_value() &&
               retryCacheSnapshot_.retryCandidate->operationId ==
                   operationId) {
        expected = retryCacheExpectedDispatch(
            *retryCacheSnapshot_.retryCandidate);
    }
    if (!expected.has_value()) {
        if (storeError) {
            *storeError = tr(
                "The verified upload does not match the durable retry record");
        }
        return false;
    }

    tryx::RetryCacheStore::VerifiedRemoteArtifact proof;
    proof.remoteName = verifiedEntry.name;
    proof.size = static_cast<qint64>(verifiedEntry.size);
    proof.source = verifiedEntry.source == 1U
        ? tryx::RetryCacheStore::RemoteArtifactSource::User
        : tryx::RetryCacheStore::RemoteArtifactSource::Preset;
    proof.readOnly = verifiedEntry.readOnly;
    const auto result = retryCacheStore().beginLocalCommit(
        retryCacheSnapshot_, *expected, proof);
    if (!result.ok() || !result.snapshot.has_value() ||
        !result.snapshot->inFlightDispatch.has_value() ||
        result.snapshot->inFlightDispatch->operationId !=
            operationId ||
        result.snapshot->inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::
                LocalCommitPending) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("The verified remote upload could not enter its durable local-commit phase")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    return true;
}

bool DeviceManager::deferRetryCacheLocalCommit(
    const QString &operationId,
    const QString &errorCategory,
    const QString &errorMessage,
    QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId !=
            operationId ||
        retryCacheSnapshot_.inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::
                LocalCommitPending) {
        if (storeError) {
            *storeError = tr(
                "The upload is not waiting for a durable local catalog commit");
        }
        return false;
    }
    const auto dispatch =
        *retryCacheSnapshot_.inFlightDispatch;
    const auto result = retryCacheStore().deferLocalCommit(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(dispatch),
        errorCategory, errorMessage);
    if (!result.ok() || !result.snapshot.has_value() ||
        !result.snapshot->retryCandidate.has_value() ||
        result.snapshot->inFlightDispatch.has_value()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("The local catalog failure could not be preserved as a durable retry")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface();
    return true;
}

bool DeviceManager::retireRetryCacheDispatch(
    const QString &operationId,
    tryx::RetryCacheStore::DispatchRetirement retirement,
    QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId !=
            operationId) {
        if (storeError) {
            *storeError = tr(
                "The operation is not the current retry dispatch");
        }
        return false;
    }
    const auto dispatch =
        *retryCacheSnapshot_.inFlightDispatch;
    const auto result = retryCacheStore().retireDispatch(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(dispatch), retirement);
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
            synchronizeRetryCacheSurface();
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("Retry dispatch could not be retired")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface();
    return true;
}

bool DeviceManager::clearRetryCacheCandidate(
    const QString &expectedOperationId) {
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.retryCandidate->operationId !=
            expectedOperationId) {
        return false;
    }
    const auto candidate =
        *retryCacheSnapshot_.retryCandidate;
    const auto result = retryCacheStore().clearCandidate(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(candidate));
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("Retry candidate could not be cleared")
            : result.detail;
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface();
    return true;
}

bool DeviceManager::consumeRetryCacheCandidate(
    const QString &expectedOperationId) {
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.retryCandidate->operationId !=
            expectedOperationId) {
        return false;
    }
    const auto candidate =
        *retryCacheSnapshot_.retryCandidate;
    const auto result = retryCacheStore().consumeCandidate(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(candidate));
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tr("Retry candidate could not be consumed")
            : result.detail;
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface();
    return true;
}

void DeviceManager::startRetryCacheReadOnlyReconciliationIfReady() {
    if (runtimeDowngradeV10Prepared_ ||
        retryCacheStartupSessionGateActive() ||
        !retryCacheRestrictedRecoveryActive() ||
        currentPrinterPath().isEmpty()) {
        return;
    }
    if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::
                ShadowMissingFence) {
        return;
    }
    const QString operationId =
        retryCacheVisibleOperationId();
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        (!activeOperationId_.isEmpty() &&
         activeOperationId_ != operationId)) {
        return;
    }
    const bool requestAlreadyCurrent =
        found->retryPreflight &&
        found->info.deviceGeneration == printerGeneration_ &&
        !found->deviceChangePending;
    if (requestAlreadyCurrent) {
        return;
    }
    const quint64 previousGeneration =
        found->info.deviceGeneration;
    found->retryPreflight = false;
    found->deviceChangePending = false;
    found->deviceChangeMessage.clear();
    found->info.deviceGeneration = printerGeneration_;
    const bool exactRecoveryDevice =
        found->printerProductId == printerProductId_ &&
        !found->uploadDeviceIdentity.trimmed().isEmpty() &&
        found->uploadDeviceIdentity.trimmed() ==
            printerDeviceSerial_.trimmed();
    if (!exactRecoveryDevice) {
        const bool finalizationCandidate =
            retryCacheSnapshot_.retryCandidate.has_value() &&
            retryCacheSnapshot_.retryCandidate->operationId ==
                operationId &&
            retryCacheSnapshot_.retryCandidate
                ->finalizationOnlyReconciliation;
        if (finalizationCandidate) {
            const auto candidate =
                *retryCacheSnapshot_.retryCandidate;
            const auto resolved = retryCacheStore()
                .resolveCandidateRecovery(
                    retryCacheSnapshot_,
                    retryCacheExpectedDispatch(candidate),
                    tryx::RetryCacheStore::CandidateRecoveryProof::
                        ReconciliationIdentityMismatch);
            if (!resolved.ok() ||
                !resolved.snapshot.has_value()) {
                retryCacheStartupFailure_ = true;
                retryCacheFailureDetail_ =
                    resolved.detail.isEmpty()
                    ? tr("The identity-mismatch recovery state could not be saved")
                    : resolved.detail;
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("RetryCacheRecoveryFailed"),
                    QString(), retryCacheFailureDetail_);
                return;
            }
            retryCacheSnapshot_ = *resolved.snapshot;
            synchronizeRetryCacheSurface();
            const QString recoveryMessage = tr(
                "The reconnected USB device does not match the PASE that accepted the upload. Power-cycle and reconnect the original device before Retry.");
            auto updated = operations_.find(operationId);
            if (updated != operations_.end()) {
                updated->info.terminalOutcome =
                    QStringLiteral("PartialOrUnknown");
                updated->requiresDeviceRecovery = true;
                updated->retryMustUseNewRemoteName = true;
                updated->uploadFinalizationReconciliationPending = false;
                if (!operationIsTerminal(updated->info.state)) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("PreparedMedia"),
                        recoveryMessage);
                } else {
                    updated->info.state =
                        QStringLiteral("RetryAvailable");
                    updated->info.stage =
                        QStringLiteral("RetryAvailable");
                    updated->info.errorCategory =
                        QStringLiteral("PartialOrUnknown");
                    updated->info.retryMode =
                        QStringLiteral("PreparedMedia");
                    updated->info.message = recoveryMessage;
                    publishOperation(operationId);
                }
            }
            requirePrinterRecovery(recoveryMessage);
            emit printerOperationsCancelled();
        }
        return;
    }
    if (activeOperationId_ == operationId) {
        emit requestEndPrinterForegroundOperation(
            operationId, previousGeneration);
        activeOperationId_.clear();
    }
    setPrinterDisplaySessionActive(false);
    stopKeepalive();
    worker_->updatePrinterGenerationGate(
        printerGeneration_, true);
    activeOperationId_ = operationId;
    found->retryPreflight = true;
    found->info.state = QStringLiteral("Refreshing");
    found->info.stage =
        QStringLiteral("RecoveringFinalization");
    found->info.message = tr(
        "Checking FileList in a restricted read-only recovery session...");
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(
        operationId, printerGeneration_);
    emit requestPrinterRefreshMedia(
        currentPrinterPath(), operationId, printerGeneration_);
}

void DeviceManager::promoteRestrictedSessionAfterProof() {
    if (retryCacheMutationGateActive() ||
        currentPrinterPath().isEmpty() ||
        printerRecoveryRequired_) {
        return;
    }
    printerDisplaySessionLost_ = false;
    printerSessionLossRemovalObserved_ = false;
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    printerSessionResumeProductId_ = 0;
    setPrinterDisplaySessionActive(true);
    const bool samplingActive =
        currentPrinterSupportsOverlayMetrics() &&
        metricsState_.enabled &&
        !metricsState_.metrics.isEmpty();
    if (metricsState_.samplingActive != samplingActive) {
        metricsState_.samplingActive = samplingActive;
        publishMetricsState();
    }
    if (currentPrinterSupportsMediaCatalog()) {
        resumePendingDeleteReconciliation();
        resumePendingReplaceReconciliation();
    }
}

bool DeviceManager::dispatchPreparedUploadWithRetryBarrier(
    const QString &devicePath, const QString &operationId,
    quint64 generation) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        found->uploadDispatched ||
        retryCacheMutationGateActive()) {
        return false;
    }

    const QString currentIdentity =
        printerDeviceSerial_.trimmed();
    const bool preDispatchIdentityCurrent =
        activeOperationId_ == operationId &&
        !found->cancelRequested &&
        !found->deviceChangePending &&
        !devicePath.isEmpty() &&
        devicePath == currentPrinterPath() &&
        generation == printerGeneration_ &&
        found->printerProductId == printerProductId_ &&
        found->uploadDeviceIdentity.trimmed() ==
            currentIdentity &&
        !currentIdentity.isEmpty() &&
        found->uploadDeviceGeneration == generation &&
        !found->preparedPath.isEmpty() &&
        QFileInfo::exists(found->preparedPath) &&
        isSha256Hex(found->preparedSha256) &&
        !found->remoteName.isEmpty();
    if (!preDispatchIdentityCurrent) {
        handlePreparedUploadFailure(
            operationId,
            tr("Prepared upload became stale before its durable dispatch barrier"),
            found->cancelRequested
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted);
        return false;
    }

    const QString stagingPreparedPath = found->preparedPath;
    const QString stagingThumbnailPath =
        found->stagedThumbnailPath;
    const bool retriesCandidate =
        found->info.kind == QStringLiteral("UploadRetry") &&
        retryCacheSnapshot_.retryCandidate.has_value() &&
        !retryCacheSnapshot_.inFlightDispatch.has_value() &&
        found->retryLineageId ==
            retryCacheSnapshot_.retryCandidate->lineageId;

    tryx::RetryCacheStore::MutationResult persisted;
    if (retriesCandidate) {
        if (found->retryDispatchId.isEmpty()) {
            found->retryDispatchId =
                QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
        }
        tryx::RetryCacheStore::RetryPreparedInput input;
        input.dispatchId = found->retryDispatchId;
        input.operationId = operationId;
        input.deviceIdentity = currentIdentity;
        input.deviceGeneration = generation;
        input.retryRemoteName = found->remoteName;
        persisted = retryCacheStore().beginRetry(
            retryCacheSnapshot_, input);
    } else {
        found->retryLineageId =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        found->retryDispatchId =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        const auto profile =
            printerProductProfileForId(
                found->printerProductId);
        if (!profile.has_value()) {
            handlePreparedUploadFailure(
                operationId,
                tr("Prepared media has no supported printer profile"),
                PrinterProtocol::MutationOutcome::NotStarted);
            return false;
        }
        tryx::RetryCacheStore::PersistPreparedInput input;
        input.lineageId = found->retryLineageId;
        input.dispatchId = found->retryDispatchId;
        input.operationId = operationId;
        input.attempt = qMax<quint32>(
            1U, found->info.attempt);
        input.productId = found->printerProductId;
        input.conversion = found->mediaConversion;
        input.deviceIdentity = currentIdentity;
        input.deviceGeneration = generation;
        input.originalRemoteName =
            found->originalRemoteName.isEmpty()
            ? found->remoteName
            : found->originalRemoteName;
        input.retryRemoteName = found->remoteName;
        input.subject = found->info.subject;
        input.primaryErrorCategory =
            found->info.primaryErrorCategory;
        input.primaryErrorMessage =
            found->info.primaryErrorMessage;
        input.prepared.stagingPath =
            found->preparedPath;
        input.prepared.expectedSize =
            QFileInfo(found->preparedPath).size();
        input.prepared.expectedSha256 =
            found->preparedSha256;
        if (!found->stagedThumbnailPath.isEmpty()) {
            const QFileInfo thumbnailInfo(
                found->stagedThumbnailPath);
            if (!thumbnailInfo.exists() ||
                !thumbnailInfo.isFile() ||
                thumbnailInfo.isSymLink() ||
                thumbnailInfo.size() <= 0 ||
                !isSha256Hex(
                    found->stagedThumbnailSha256)) {
                handlePreparedUploadFailure(
                    operationId,
                    tr("Prepared thumbnail failed the durable dispatch preflight"),
                    PrinterProtocol::MutationOutcome::NotStarted);
                return false;
            }
            input.thumbnail =
                tryx::RetryCacheStore::PreparedArtifactInput{
                    found->stagedThumbnailPath,
                    thumbnailInfo.size(),
                    found->stagedThumbnailSha256,
                };
        }
        if (isSha256Hex(
                found->sourceContentSha256) &&
            found->sourceSize > 0 &&
            !found->conversionProfile.isEmpty()) {
            input.origin =
                tryx::RetryCacheStore::OriginIdentity{
                    found->sourceContentSha256,
                    found->sourceSize,
                    found->conversionProfile,
                };
        }
        persisted = retryCacheStore().persistPrepared(
            retryCacheSnapshot_, input);
    }

    if (!persisted.ok() ||
        !persisted.snapshot.has_value() ||
        !persisted.snapshot->inFlightDispatch.has_value()) {
        if (!retriesCandidate) {
            releasePrinterPreparationPath(stagingPreparedPath);
            releasePrinterPreparationPath(stagingThumbnailPath);
        }
        retryCacheStartupFailure_ =
            retryCacheStore().blocksMutations();
        retryCacheFailureDetail_ =
            persisted.detail;
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("RetryCacheWriteFailed"),
            QString(),
            persisted.detail.isEmpty()
                ? tr("Prepared upload was stopped before USB because its durable state could not be saved")
                : tr("Prepared upload was stopped before USB: %1")
                      .arg(persisted.detail));
        return false;
    }

    retryCacheSnapshot_ = *persisted.snapshot;
    const auto dispatch =
        *retryCacheSnapshot_.inFlightDispatch;
    found = operations_.find(operationId);
    if (found == operations_.end()) {
        return false;
    }
    found->retryLineageId = dispatch.lineageId;
    found->retryDispatchId = dispatch.dispatchId;
    found->preparedPath =
        retryCacheArtifactPath(dispatch.prepared);
    found->preparedSha256 = dispatch.prepared.sha256;
    found->stagedThumbnailPath =
        dispatch.thumbnail.has_value()
        ? retryCacheArtifactPath(*dispatch.thumbnail)
        : QString();
    found->stagedThumbnailSha256 =
        dispatch.thumbnail.has_value()
        ? dispatch.thumbnail->sha256
        : QString();
    found->info.total = dispatch.prepared.size;
    if (!retriesCandidate) {
        if (stagingPreparedPath !=
            found->preparedPath) {
            releasePrinterPreparationPath(stagingPreparedPath);
        }
        if (!stagingThumbnailPath.isEmpty() &&
            stagingThumbnailPath !=
                found->stagedThumbnailPath) {
            releasePrinterPreparationPath(stagingThumbnailPath);
        }
    }
    synchronizeRetryCacheSurface();

    const auto armed = retryCacheStore().armDispatch(
        retryCacheSnapshot_,
        retryCacheExpectedDispatch(dispatch));
    if (!armed.ok() || !armed.snapshot.has_value() ||
        !armed.snapshot->inFlightDispatch.has_value() ||
        armed.snapshot->inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::
                DispatchArmed) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = armed.detail;
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("RetryCacheArmFailed"),
            QString(),
            armed.detail.isEmpty()
                ? tr("Prepared upload was stopped before USB because its shadow barrier could not be armed")
                : tr("Prepared upload was stopped before USB: %1")
                      .arg(armed.detail));
        return false;
    }
    retryCacheSnapshot_ = *armed.snapshot;

    found = operations_.find(operationId);
    if (found != operations_.end()) {
        found->info.state = QStringLiteral("Uploading");
        found->info.stage = QStringLiteral("Uploading");
        found->info.message =
            tr("Transferring prepared media to the PASE...");
        publishOperation(operationId);
    }

    // operationChanged() is synchronous. Re-find and revalidate after the
    // Uploading publication so a reentrant cancel or device change cannot
    // slip between the last identity proof and the sole USB dispatch emit.
    found = operations_.find(operationId);
    const QString canonicalPreparedPath =
        retryCacheArtifactPath(
            retryCacheSnapshot_.inFlightDispatch->prepared);
    const QString uploadPreparedPath =
        found != operations_.end() ? found->preparedPath : QString();
    const QString uploadRemoteName =
        found != operations_.end() ? found->remoteName : QString();
    const QString uploadPreparedSha256 =
        found != operations_.end() ? found->preparedSha256 : QString();
    const bool finalIdentityCurrent =
        found != operations_.end() &&
        !operationIsTerminal(found->info.state) &&
        activeOperationId_ == operationId &&
        !found->cancelRequested &&
        !found->deviceChangePending &&
        !found->uploadDispatched &&
        devicePath == currentPrinterPath() &&
        generation == printerGeneration_ &&
        found->printerProductId == printerProductId_ &&
        found->uploadDeviceIdentity.trimmed() ==
            printerDeviceSerial_.trimmed() &&
        !printerDeviceSerial_.trimmed().isEmpty() &&
        found->uploadDeviceGeneration == generation &&
        found->preparedPath == canonicalPreparedPath &&
        found->preparedSha256 ==
            retryCacheSnapshot_.inFlightDispatch
                ->prepared.sha256;
    if (!finalIdentityCurrent) {
        const bool cancelled =
            found != operations_.end() &&
            found->cancelRequested;
        QString retirementError;
        if (!retireRetryCacheDispatch(
                operationId,
                cancelled
                    ? tryx::RetryCacheStore::
                          DispatchRetirement::
                              ProvenCancelled
                    : tryx::RetryCacheStore::
                          DispatchRetirement::
                              ProvenNotStarted,
                &retirementError)) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheRetirementFailed"),
                QString(),
                retirementError.isEmpty()
                    ? tr("USB dispatch was stopped, but durable retry state could not be retired")
                    : retirementError);
            return false;
        }
        finishOperation(
            operationId,
            cancelled ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            cancelled ? QStringLiteral("UserCancelled")
                      : QStringLiteral("DeviceChanged"),
            QString(),
            cancelled
                ? tr("Operation cancelled before USB dispatch")
                : tr("USB identity changed before dispatch"));
        return false;
    }

    found->uploadDispatched = true;
    emit requestPrinterUploadPrepared(
        devicePath, uploadPreparedPath, uploadRemoteName,
        uploadPreparedSha256, operationId, generation);
    return true;
}
void DeviceManager::setBrightness(int value) {
    const int boundedValue = qBound(0, value, 100);
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.display.brightnessPresent = true;
        request.display.brightness = boundedValue;
        queueApplyOperation(QString(), request);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestBrightness(boundedValue);
}

void DeviceManager::setScreenConfig(
    const QStringList &media, const QString &ratio,
    const QString &screenMode, const QString &playMode,
    const QStringList &sysinfoLabels, const QString &settingsPosition,
    const QString &settingsColor, const QString &settingsAlign,
    const QStringList &settingsBadges, int filterOpacity,
    const QString &presetId, const QStringList &sysinfoLabels2,
    const QStringList &settingsBadges2, bool waterfallMode) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.media = media;
        request.ratio = ratio;
        request.screenMode = screenMode;
        request.playMode = playMode;
        request.sysinfoLabels = sysinfoLabels;
        request.settingsPosition = settingsPosition;
        request.settingsColor = settingsColor;
        request.settingsAlign = settingsAlign;
        request.settingsBadges = settingsBadges;
        request.filterOpacity = filterOpacity;
        request.presetId = presetId;
        request.sysinfoLabels2 = sysinfoLabels2;
        request.settingsBadges2 = settingsBadges2;
        request.waterfallMode = waterfallMode;
        request.settingsPosition2 = settingsPosition;
        request.settingsColor2 = settingsColor;
        request.settingsAlign2 = settingsAlign;
        request.replaceOverlay = true;
        request.display.orientationPresent = true;
        request.display.waterfallMode = waterfallMode;
        queueApplyOperation(QString(), request, true);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }

    if (!connected_) {
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestScreenConfig(media, ratio, screenMode, playMode,
                             sysinfoLabels, settingsPosition, settingsColor,
                             settingsAlign, settingsBadges, filterOpacity,
                             presetId, sysinfoLabels2, settingsBadges2,
                             waterfallMode);
}

void DeviceManager::sendSysinfo(const QStringList &labels,
                                const QStringList &values,
                                const QStringList &units) {
    if (firmwareExclusiveActive()) {
        return;
    }
    if (isPrinterClassDevicePresent()) {
        if (!currentPrinterSupportsOverlayMetrics()) {
            emit deviceError(tr(
                "Overlay metrics are not supported for USB product %1")
                                 .arg(printerProductIdString(
                                     printerProductId_)));
            return;
        }
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty() || !printerDisplaySessionActive_ ||
            retryCacheMutationGateActive() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_ ||
            !activeOperationId_.isEmpty()) {
            return;
        }
        emit requestPrinterSysinfo(devicePath, labels, values, units,
                                   printerGeneration_);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestSysinfo(labels, values, units);
}

void DeviceManager::setRotation(int degrees) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Rotation is not supported on printer-class firmware yet."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestRotation(degrees);
}

void DeviceManager::rebootDevice() {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Reboot is not supported on printer-class firmware yet."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestReboot();
}

void DeviceManager::deleteMedia(const QStringList &files) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Media deletion is disabled because USB file_remove has no dedicated response."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestDeleteMedia(files);
}

void DeviceManager::uploadMedia(const QString &localPath) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        queueUploadOperation(QString(), localPath, false);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (!connected_) {
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestUploadMedia(localPath);
}

void DeviceManager::refreshMediaList() {
    if (runtimeDowngradeV10Prepared_) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        if (!currentPrinterSupportsMediaCatalog()) {
            emit deviceError(tr(
                "Media catalog refresh is not supported for USB product %1")
                                 .arg(printerProductIdString(
                                     printerProductId_)));
            return;
        }
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty()) {
            emit deviceError(printerUnavailableStatusText());
            return;
        }
        if (retryCacheMutationGateActive() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_) {
            emit deviceError(printerMutationUnavailableStatusText());
            return;
        }
        if (!activeOperationId_.isEmpty()) {
            emit deviceError(
                tr("FileList refresh is deferred while operation %1 is active")
                    .arg(activeOperationId_));
            return;
        }
        emit requestPrinterRefreshMedia(devicePath, QString(),
                                        printerGeneration_);
        return;
    }
    if (!connected_) {
        emit mediaListUpdated({});
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestRefreshMedia();
}

void DeviceManager::startKeepalive(int intervalSec) {
    if (runtimeDowngradeV10Prepared_ ||
        firmwareExclusiveActive()) {
        return;
    }
    if (printerClassConnected_) {
        keepaliveTimer_->stop();
        return;
    }
    keepaliveTimer_->start(qMax(1, intervalSec) * 1000);
}

void DeviceManager::stopKeepalive() {
    keepaliveTimer_->stop();
}
