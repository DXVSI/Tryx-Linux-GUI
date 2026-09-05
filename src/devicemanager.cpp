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
using tryx::private_runtime_paths::ensurePrivateDirectory;
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
constexpr qint64 kMaxRetryCacheBytes =
    tryx::printer_media_file_integrity::kMaximumPreparedMediaBytes;
constexpr qint64 kMaxThumbnailBytes =
    tryx::printer_media_file_integrity::kMaximumThumbnailBytes;
constexpr auto kRetryCacheTransitionConflictId =
    "retry-cache-transition-conflict";
constexpr qint64 kFileTransmitChunkSize = 0x40000;
constexpr int kDeviceMediaSweepIntervalMs = 5000;
constexpr qint64 kRecoveredMediaFreeSpaceReserveBytes =
    16LL * 1024LL * 1024LL;
constexpr quint16 kTurrisProductId = 0x2011;

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
    if (!operationCoordinator_.activeOperationId().isEmpty()) {
        return fail(
            tr("Device operation %1 is still active")
                .arg(operationCoordinator_.activeOperationId()));
    }
    if (retryCacheMutationGateActive()) {
        return fail(tr(
            "Stored retry media is still being validated or requires recovery"));
    }
    if (operationCoordinator_.hasUnresolvedRetryOutcomeForFirmware()) {
        return fail(tr(
            "A previous media transfer has an unresolved device outcome; cancel or reconcile it before firmware flashing"));
    }
    if (operationCoordinator_.hasPendingDeleteRecovery()) {
        return fail(tr(
            "A previous delete command still requires read-only reconciliation"));
    }
    if (operationCoordinator_.hasPendingReplaceRecovery()) {
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
      paseMetricsConfigStore_(
          std::make_unique<tryx::PaseMetricsConfigStore>()),
      runtimePresentationPreferencesStore_(
          std::make_unique<tryx::RuntimePresentationPreferencesStore>()),
      savedLayoutStore_(
          std::make_unique<tryx::SavedLayoutStore>()),
      runtimeDowngradeStore_(
          std::make_unique<tryx::RuntimeDowngradeStore>()) {
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

    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationChanged,
        this, &DeviceManager::operationChanged,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationRemoved,
        this, &DeviceManager::operationRemoved,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaCatalogUpdated,
        this, &DeviceManager::mediaCatalogUpdated,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaListUpdated,
        this, &DeviceManager::mediaListUpdated,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaUploaded,
        this, &DeviceManager::mediaUploaded,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationError,
        this, &DeviceManager::deviceError,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestEndForegroundOperation,
        this, &DeviceManager::requestEndPrinterForegroundOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestClearWorkerCancellation,
        this,
        [this](const QString &operationId) {
            if (worker_) {
                worker_->clearPrinterOperationCancellation(operationId);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestApplyDeferredMediaCatalog,
        this,
        [this](const QList<PrinterProtocol::MediaFile> &mediaFiles,
               quint64 generation, const QString &deviceIdentity) {
            if (printerGeneration_ == generation &&
                printerDeviceSerial_.trimmed() == deviceIdentity) {
                updateMediaCatalog(mediaFiles);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestArtifactSweep,
        this, &DeviceManager::sweepDeviceMediaArtifacts,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestUnwatchArtifactOwner,
        this,
        [this](const QString &ownerUniqueName) {
            if (artifactOwnerWatcher_) {
                artifactOwnerWatcher_->removeWatchedService(
                    ownerUniqueName);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestWatchArtifactOwner,
        this,
        [this](const QString &ownerUniqueName, bool *registered) {
            if (registered) {
                *registered = watchArtifactOwner(ownerUniqueName);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestBeginForegroundOperation,
        this, &DeviceManager::requestBeginPrinterForegroundOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestStageMedia,
        this, &DeviceManager::requestPrinterStageMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelOperation,
        this, &DeviceManager::cancelOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestReplacePreflight,
        this, &DeviceManager::requestPrinterReplacePreflight,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRecoveredMedia,
        this, &DeviceManager::requestPrepareRecoveredPrinterMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRecoveredMediaWithProfile,
        this,
        &DeviceManager::
            requestPrepareRecoveredPrinterMediaWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestAnalyzeSource,
        this, &DeviceManager::requestAnalyzePrinterSource,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestAnalyzeSourceWithProfile,
        this,
        &DeviceManager::requestAnalyzePrinterSourceWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareMedia,
        this, &DeviceManager::requestPreparePrinterMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareMediaWithProfile,
        this,
        &DeviceManager::requestPreparePrinterMediaWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestDeleteMedia,
        this, &DeviceManager::requestPrinterDeleteMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestApplyMedia,
        this, &DeviceManager::requestPrinterApplyMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestConfigureMetrics,
        this, &DeviceManager::requestPrinterConfigureMetrics,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestRefreshMedia,
        this, &DeviceManager::requestPrinterRefreshMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestDispatchPreparedUpload,
        this,
        [this](const QString &devicePath, const QString &operationId,
               quint64 generation) {
            operationCoordinator_.dispatchPreparedUploadWithRetryBarrier(
                operationContext(), devicePath, operationId, generation);
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestUploadPrepared,
        this, &DeviceManager::requestPrinterUploadPrepared,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrinterRecovery,
        this, &DeviceManager::requirePrinterRecovery,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestStartRetryCacheReadOnlyReconciliation,
        this,
        [this]() {
            startRetryCacheReadOnlyReconciliationIfReady();
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestResumePrinterSessionAfterRetryCacheValidation,
        this,
        &DeviceManager::resumePrinterSessionAfterRetryCacheValidation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestValidateRetryCacheArtifact,
        this, &DeviceManager::requestValidatePrinterRetryCacheArtifact,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelRetryCacheValidation,
        this,
        [this](const QString &validationToken) {
            if (printerMediaPreparer_) {
                printerMediaPreparer_->cancelRetryValidation(
                    validationToken);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestClearRetryCacheValidationCancellation,
        this,
        [this](const QString &validationToken) {
            if (printerMediaPreparer_) {
                printerMediaPreparer_->clearRetryValidationCancellation(
                    validationToken);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRestrictedReadOnlySession,
        this,
        [this](quint64 generation) {
            setPrinterDisplaySessionActive(false);
            stopKeepalive();
            if (worker_) {
                worker_->updatePrinterGenerationGate(generation, true);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationsCancelled,
        this, &DeviceManager::printerOperationsCancelled,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestPromoteRestrictedSessionAfterProof,
        this, &DeviceManager::promoteRestrictedSessionAfterProof,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelPreparationOperation,
        this, &DeviceManager::requestCancelPrinterPreparationOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestReleasePreparationPath,
        this, &DeviceManager::requestReleasePrinterPreparation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelWorkerOperation,
        this,
        [this](const QString &operationId) {
            if (worker_) {
                worker_->cancelPrinterOperation(operationId);
            }
        },
        Qt::DirectConnection);

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

                if (!operationCoordinator_.handleSessionStarted(
                        operationContext())) {
                    return;
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
                if (!operationCoordinator_.handleSessionLostBeforeStateChange(
                        operationContext())) {
                    return;
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
            [this](const QString &operationId, const QString &stage,
                   qint64 completed, qint64 total, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handleForegroundProgress(
                    operationContext(), operationId, stage, completed,
                    total, message, generation);
            });
    connect(worker_, &DeviceWorker::printerMediaStaged, this,
            [this](const QString &operationId, const QString &mediaName,
                   const QString &outputPath, bool success, bool cancelled,
                   qint64 fileSize, qint64 chunkCount,
                   const QString &rawSha256,
                   const QString &decodedSha256,
                   const RecoveredH264ProbeMetadata &probeMetadata,
                   const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleMediaStaged(
                    operationContext(), operationId, mediaName, outputPath,
                    success, cancelled, fileSize, chunkCount, rawSha256,
                    decodedSha256, probeMetadata, errorMessage, generation);
            });
    connect(
        worker_, &DeviceWorker::printerReplacePreflightFinished,
        this,
        [this](
            const QString &operationId, const QString &mediaName,
            const QString &expectedReplacementName,
            qint64 expectedReplacementSize,
            const QStringList &references,
            const QStringList &referencingSlots,
            const QString &activeScreenMode,
            const QString &activePlayMode,
            const QStringList &activeMedia,
            bool originalIdentityVerified,
            bool replacementIdentityVerified, bool success,
            const QString &errorMessage, quint64 generation) {
            operationCoordinator_.handleReplacePreflightFinished(
                operationContext(), operationId, mediaName,
                expectedReplacementName, expectedReplacementSize,
                references, referencingSlots, activeScreenMode,
                activePlayMode, activeMedia, originalIdentityVerified,
                replacementIdentityVerified, success, errorMessage,
                generation);
        });

    connect(worker_, &DeviceWorker::printerUploadFinished, this,
            [this](
                const QString &operationId, const QString &uploadPath,
                const QString &remoteName, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleUploadFinished(
                    operationContext(), operationId, uploadPath,
                    remoteName, success, outcome, errorMessage,
                    generation);
            });
    connect(worker_, &DeviceWorker::printerMediaListReady, this,
            [this](
                const QString &operationId,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                quint64 generation) {
                operationCoordinator_.handleMediaListReady(
                    operationContext(), operationId, mediaFiles,
                    generation);
            });

    connect(worker_, &DeviceWorker::printerMediaListFailed, this,
            [this](
                const QString &operationId, const QString &message,
                quint64 generation) {
                operationCoordinator_.handleMediaListFailed(
                    operationContext(), operationId, message,
                    generation);
            });
    connect(worker_, &DeviceWorker::printerDeleteFinished, this,
            [this](
                const QString &operationId,
                const QStringList &requestedNames,
                const QStringList &deletedNames,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleDeleteFinished(
                    operationContext(), operationId, requestedNames,
                    deletedNames, mediaFiles, success, outcome,
                    errorMessage, generation);
            });
    connect(worker_, &DeviceWorker::printerSavedLayoutProofFailed, this,
            [this](
                const QString &operationId,
                const QString &errorCategory,
                const QString &errorMessage,
                quint64 generation) {
                operationCoordinator_.handleSavedLayoutProofFailed(
                    operationContext(), operationId, errorCategory,
                    errorMessage, generation);
            });
    connect(worker_, &DeviceWorker::printerApplyFinished, this,
            [this](
                const QString &operationId, const QString &mediaFile,
                bool success, bool metricsUpdated,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleApplyFinished(
                    operationContext(), operationId, mediaFile, success,
                    metricsUpdated, outcome, errorMessage, generation,
                    [this](const QString &deviceIdentity) {
                        return persistedPaseOverlayForDevice(
                            deviceIdentity);
                    },
                    [this](
                        const PrinterProtocol::PaseOverlayConfig &overlay,
                        bool enabled, QString *persistenceError) {
                        return persistPaseMetricsConfiguration(
                            overlay, enabled, persistenceError);
                    },
                    [this](
                        const PrinterProtocol::PaseOverlayConfig &overlay,
                        bool enabled, const QString &diagnostic,
                        bool updateDisplay) {
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled = enabled;
                        metricsState_.samplingActive = enabled;
                        metricsState_.metrics = overlay.left.metrics;
                        metricsState_.alignment = overlay.left.alignment;
                        metricsState_.textColor = overlay.left.textColor;
                        metricsState_.diagnostic = diagnostic;
                        publishMetricsState();
                        if (!updateDisplay || !displayState_.valid) {
                            return;
                        }
                        PrinterProtocol::PaseDisplayState state;
                        state.backlightEnabled =
                            displayState_.backlightEnabled;
                        state.brightness = displayState_.brightness;
                        state.standbyEnabled =
                            displayState_.standbyEnabled;
                        state.standbyMedia = displayState_.standbyMedia;
                        state.mirrorMode = displayState_.mirrorMode;
                        state.waterfallMode = displayState_.waterfallMode;
                        state.screenMode = displayState_.screenMode;
                        state.playMode = displayState_.playMode;
                        state.media = displayState_.media;
                        updateDisplayState(state, overlay);
                    },
                    [this]() { emit screenConfigChanged(); });
            });
    connect(worker_, &DeviceWorker::printerMetricsConfigured, this,
            [this](
                const QString &operationId, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleMetricsConfigured(
                    operationContext(), operationId, success, outcome,
                    errorMessage, generation,
                    [this](
                        const PrinterProtocol::PaseOverlayConfig &overlay,
                        bool enabled, QString *persistenceError) {
                        return persistPaseMetricsConfiguration(
                            overlay, enabled, persistenceError);
                    },
                    [this](
                        const PrinterProtocol::PaseOverlayConfig &overlay,
                        bool enabled, const QString &diagnostic,
                        bool updateDisplay) {
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled = enabled;
                        metricsState_.samplingActive = enabled;
                        metricsState_.metrics = overlay.left.metrics;
                        metricsState_.alignment = overlay.left.alignment;
                        metricsState_.textColor = overlay.left.textColor;
                        metricsState_.diagnostic = diagnostic;
                        publishMetricsState();
                        if (!updateDisplay || !displayState_.valid) {
                            return;
                        }
                        PrinterProtocol::PaseDisplayState state;
                        state.backlightEnabled =
                            displayState_.backlightEnabled;
                        state.brightness = displayState_.brightness;
                        state.standbyEnabled =
                            displayState_.standbyEnabled;
                        state.standbyMedia = displayState_.standbyMedia;
                        state.mirrorMode = displayState_.mirrorMode;
                        state.waterfallMode = displayState_.waterfallMode;
                        state.screenMode = displayState_.screenMode;
                        state.playMode = displayState_.playMode;
                        state.media = displayState_.media;
                        updateDisplayState(state, overlay);
                    });
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
                    operationCoordinator_.activeOperationId().isEmpty()) {
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
            [this](
                const QString &operationId, const QString &localPath,
                const QString &contentSha256, qint64 sourceSize,
                const QString &conversionProfile, quint64 generation) {
                operationCoordinator_.handleSourceAnalyzed(
                    operationContext(), operationId, localPath,
                    contentSha256, sourceSize, conversionProfile,
                    generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::progress, this,
            [this](const QString &operationId, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handlePreparationProgress(
                    operationContext(), operationId, message, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::failed, this,
            [this](const QString &operationId, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handlePreparationFailed(
                    operationContext(), operationId, message, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::prepared, this,
            [this](
                const QString &operationId, const QString &devicePath,
                const QString &sourcePath, const QString &uploadPath,
                const QString &remoteName, const QString &preparedSha256,
                const QString &stagedThumbnailPath,
                const QString &stagedThumbnailSha256, quint64 generation) {
                operationCoordinator_.handlePrepared(
                    operationContext(), operationId, devicePath,
                    sourcePath, uploadPath, remoteName, preparedSha256,
                    stagedThumbnailPath, stagedThumbnailSha256,
                    generation);
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
    manager->operationCoordinator_.retryCacheDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("retry-cache"));
    manager->operationCoordinator_.mediaCatalogStore_ =
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
    manager->operationCoordinator_.mediaRuntimeRootOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("runtime-staging"));
    manager->operationCoordinator_.deviceMediaArtifactStore_ =
        std::make_unique<tryx::DeviceMediaArtifactStore>(
            QDir(manager->operationCoordinator_.mediaRuntimeRootOverride_)
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
    operationCoordinator_.cancelPendingRetryCacheValidations();
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
    operationCoordinator_.shutdownAfterWorkersStopped();
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

PrinterOperationContext DeviceManager::operationContext() const {
    PrinterOperationContext context;
    context.devicePath = currentPrinterPath();
    context.deviceIdentity = printerDeviceSerial_.trimmed();
    context.unavailableStatusText = printerUnavailableStatusText();
    context.mutationUnavailableStatusText =
        printerMutationUnavailableStatusText();
    context.firmwareExclusiveStatusText =
        firmwareExclusiveStatusText();
    context.productId = printerProductId_;
    context.generation = printerGeneration_;
    context.connected = connected_;
    context.printerClassConnected = printerClassConnected_;
    context.printerEndpointReady =
        printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready;
    context.displaySessionActive = printerDisplaySessionActive_;
    context.displaySessionLost = printerDisplaySessionLost_;
    context.recoveryRequired = printerRecoveryRequired_;
    context.runtimeDowngradePrepared = runtimeDowngradeV10Prepared_;
    context.firmwareExclusiveActive = firmwareExclusiveActive();
    context.firmwareReleasePending =
        !firmwareReleasePendingLeaseId_.isEmpty();
    context.firmwareRecoveryInterlockActive =
        firmwareRecoveryInterlockActive_;
    const auto profile = currentPrinterProductProfile();
    if (profile) {
        context.supportsMediaCatalog =
            profile->mediaCatalogSupported;
        context.supportsDisplayConfiguration =
            profile->displayConfigurationSupported;
        context.supportsOverlayMetrics =
            profile->overlayMetricsSupported;
        context.supportsSplitAreaMedia =
            profile->splitAreaMediaSupported;
    }
    context.displayState = displayState_;
    context.displayStateGeneration = displayStateReadGeneration_;
    return context;
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
    if (operationCoordinator_.retryCacheValidationPending()) {
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
    PrinterOperationContext recoveryContext = operationContext();
    recoveryContext.deviceIdentity = currentDeviceIdentity.trimmed();
    recoveryContext.productId = currentProductId;
    if (!operationCoordinator_.completeRetryRecoveryAfterRemoval(
            recoveryContext)) {
        return false;
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
    return operationCoordinator_.normalizedOperationId(requestedId);
}

void DeviceManager::cleanupMediaRuntimeStaging() {
    operationCoordinator_.cleanupMediaRuntimeStaging();
}

void DeviceManager::cleanupDeviceMediaOutbox() {
    operationCoordinator_.initializeDeviceMediaOutbox();
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
    operationCoordinator_.handleArtifactOwnerUnregistered(
        operationContext(), ownerUniqueName);
}

void DeviceManager::sweepDeviceMediaArtifacts() {
    operationCoordinator_.sweepDeviceMediaArtifacts(operationContext());
}

TryxRuntimeOperationsSnapshot DeviceManager::operationSnapshot() const {
    return operationCoordinator_.operationSnapshot();
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
    const auto operationReadiness =
        operationCoordinator_.runtimeDowngradeAssessment();
    if (operationReadiness.activeOperationPresent) {
        return fail(tr(
            "A device operation is still active; wait for it to finish before preparing a runtime downgrade"));
    }
    if (operationReadiness.pendingOperationPresent) {
        return fail(tr(
            "A device operation is still pending; wait for it to finish before preparing a runtime downgrade"));
    }
    if (operationReadiness.recoveryPending) {
        return fail(tr(
            "Delete or replacement recovery must finish before preparing a runtime downgrade"));
    }
    if (!operationReadiness.retryCacheReady) {
        return fail(tr(
            "Stored retry media is not fully validated for runtime downgrade"));
    }

    if (!operationReadiness.retryCompatible) {
        qWarning().noquote()
            << "Runtime downgrade v10 preparation blocked:"
            << operationReadiness.retryCompatibilityStatus;
        return fail(tr(
            "Stored retry media is not exactly compatible with runtime v10"));
    }
    const QString compatibilityMode =
        operationReadiness.retryCompatibilityStatus ==
                QStringLiteral("SafeEmpty")
        ? QStringLiteral("Empty")
        : operationReadiness.retryCompatibilityStatus ==
                QStringLiteral("SafeFullRetry")
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

    const auto finalOperationReadiness =
        operationCoordinator_.runtimeDowngradeAssessment();
    if (!finalOperationReadiness.retryCacheReady ||
        !finalOperationReadiness.retryCompatible ||
        finalOperationReadiness.retryCompatibilityStatus !=
            operationReadiness.retryCompatibilityStatus) {
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
        compatibilityMode, finalOperationReadiness.retryStoreRevision,
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
    const auto operationState = operationCoordinator_.supportState();
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
    source.mediaCatalogEntryCount = operationState.mediaCatalogEntryCount;
    source.artifactCount = operationState.artifactCount;
    source.operationCount = operationState.operationCount;
    source.retryCandidatePresent = operationState.retryCandidatePresent;
    source.retryDispatchPresent = operationState.retryDispatchPresent;
    source.retryCleanupPendingCount =
        operationState.retryCleanupPendingCount;
    source.deleteRecoveryPresent = operationState.deleteRecoveryPresent;
    source.replaceRecoveryPresent = operationState.replaceRecoveryPresent;
    source.operations = operationState.recentOperations;
    return tryx::buildSupportSnapshotV1(source);
}

TryxRuntimeOperationInfo DeviceManager::operationInfo(
    const QString &operationId) const {
    return operationCoordinator_.operationInfo(operationId);
}

TryxRuntimeOperationInfo DeviceManager::activeOperationInfo() const {
    return operationCoordinator_.activeOperationInfo();
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
    return operationCoordinator_.mediaCatalogSnapshot();
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
    return operationCoordinator_.mediaCatalogDirectory();
}

QString DeviceManager::mediaThumbnailPath(
    const QString &thumbnailKey) const {
    return operationCoordinator_.mediaThumbnailPath(thumbnailKey);
}

void DeviceManager::loadMediaCatalogStore() {
    operationCoordinator_.loadMediaCatalogStore();
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
    const TryxRuntimeMediaCatalogSnapshot mediaCatalog =
        operationCoordinator_.mediaCatalogSnapshot();
    if (!proof ||
        !currentSavedLayoutsContext(&deviceIdentity, &productId) ||
        mediaCatalog.deviceIdentity != deviceIdentity) {
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
        for (const TryxRuntimeMediaEntry &entry : mediaCatalog.entries) {
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
    operationCoordinator_.updateMediaCatalog(
        operationContext(), mediaFiles);
}

void DeviceManager::clearMediaCatalogView() {
    operationCoordinator_.clearMediaCatalogView();
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

void DeviceManager::rejectOperation(const QString &operationId,
                                    const QString &kind,
                                    const QString &subject,
                                    const QString &category,
                                    const QString &message) {
    operationCoordinator_.rejectOperation(
        operationId, kind, subject, category, message,
        printerGeneration_);
}

void DeviceManager::rejectSavedLayoutApplyOperation(
    const QString &operationId, const QString &subject,
    const QString &category, const QString &message) {
    operationCoordinator_.rejectOperation(
        operationId, QStringLiteral("SavedLayoutApply"),
        subject, category, message, printerGeneration_,
        QStringLiteral("NotStarted"));
}

QString DeviceManager::queueStageDeviceMediaOperation(
    const QString &requestedOperationId, const QString &mediaId,
    const QString &ownerUniqueName) {
    return operationCoordinator_.queueStageDeviceMediaOperation(
        operationContext(), requestedOperationId, mediaId,
        ownerUniqueName);
}

TryxRuntimeDeviceMediaArtifact
DeviceManager::claimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.claimDeviceMediaArtifact(
        operationContext(), operationId, artifactId, ownerUniqueName,
        errorMessage);
}

TryxRuntimeDeviceMediaMetadataV1
DeviceManager::deviceMediaMetadataV1(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) const {
    return operationCoordinator_.deviceMediaMetadataV1(
        artifactId, leaseId, ownerUniqueName, errorMessage);
}

bool DeviceManager::renewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.renewDeviceMediaArtifactLease(
        operationContext(), artifactId, leaseId, ownerUniqueName,
        errorMessage);
}

bool DeviceManager::releaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.releaseDeviceMediaArtifact(
        operationContext(), artifactId, leaseId, ownerUniqueName,
        errorMessage);
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
    return operationCoordinator_.queueRecoveredOperation(
        operationContext(), requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, replace, originalMediaId,
        applyRequest);
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
    return operationCoordinator_.queueUploadOperation(
        operationContext(), requestedOperationId, localPath,
        applyAfterUpload, applyRequest, updateMetrics, ensureExisting,
        profile);
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
    return operationCoordinator_.queueDeleteMediaOperation(
        operationContext(), requestedOperationId, fileNames);
}

QString DeviceManager::queueApplyOperation(
    const QString &requestedOperationId,
    const TryxRuntimeApplyRequest &request, bool updateMetrics,
    const QString &proofDeviceIdentity,
    const QList<TryxRuntimeSavedMediaRefV1> &proof,
    bool savedLayoutApply) {
    return operationCoordinator_.queueApplyOperation(
        operationContext(), requestedOperationId, request, updateMetrics,
        proofDeviceIdentity, proof, savedLayoutApply);
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
    if (!operationCoordinator_.operationInfo(operationId).id.isEmpty()) {
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

QString DeviceManager::queueCacheCleanupOperation(
    const QString &requestedOperationId, QString *errorName,
    QString *errorMessage) {
    return operationCoordinator_.queueCacheCleanupOperation(
        operationContext(), requestedOperationId, errorName,
        errorMessage);
}

QString DeviceManager::queueMetricsConfigOperation(
    const QString &requestedOperationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    return operationCoordinator_.queueMetricsConfigOperation(
        operationContext(), requestedOperationId, request);
}

QString DeviceManager::retryOperation(
    const QString &sourceOperationId,
    const QString &requestedNewOperationId) {
    return operationCoordinator_.retryOperation(
        operationContext(), sourceOperationId, requestedNewOperationId);
}

void DeviceManager::cancelOperation(const QString &operationId) {
    operationCoordinator_.cancelOperation(operationContext(), operationId);
}

void DeviceManager::cancelForegroundForGenerationChange(
    const QString &message) {
    operationCoordinator_.cancelForegroundForGenerationChange(
        operationContext(), message);
}


bool DeviceManager::releasePrinterPreparationPath(
    const QString &path) {
    return operationCoordinator_.releasePrinterPreparationPath(path);
}

void DeviceManager::loadReplaceJournal() {
    operationCoordinator_.loadReplaceJournal();
}

void DeviceManager::resumePendingReplaceReconciliation() {
    operationCoordinator_.resumePendingReplaceReconciliation(
        operationContext());
}

void DeviceManager::loadDeleteIntent() {
    operationCoordinator_.loadDeleteIntent();
}

void DeviceManager::resumePendingDeleteReconciliation() {
    operationCoordinator_.resumePendingDeleteReconciliation(
        operationContext());
}

bool DeviceManager::retryCacheStoreBlocksMutations() const {
    return operationCoordinator_.retryCacheStoreBlocksMutations();
}

bool DeviceManager::retryCacheStartupSessionGateActive() const {
    return operationCoordinator_.retryCacheStartupSessionGateActive();
}

bool DeviceManager::retryCacheRestrictedRecoveryActive() const {
    return operationCoordinator_.retryCacheRestrictedRecoveryActive();
}

bool DeviceManager::retryCacheMutationGateActive() const {
    return operationCoordinator_.retryCacheMutationGateActive(
        operationContext());
}

void DeviceManager::loadRetryCache() {
    operationCoordinator_.loadRetryCache(operationContext());
}

void DeviceManager::handleRetryCacheArtifactValidation(
    const QString &validationToken, bool valid,
    bool cancelled, qint64 actualSize,
    const QString &actualSha256, quint64 actualDevice,
    quint64 actualInode, const QString &message) {
    operationCoordinator_.handleRetryCacheArtifactValidation(
        operationContext(), validationToken, valid, cancelled,
        actualSize, actualSha256, actualDevice, actualInode, message);
}


void DeviceManager::startRetryCacheReadOnlyReconciliationIfReady() {
    operationCoordinator_.startRetryCacheReadOnlyReconciliationIfReady(
        operationContext());
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
            !operationCoordinator_.activeOperationId().isEmpty()) {
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
        if (!operationCoordinator_.activeOperationId().isEmpty()) {
            emit deviceError(
                tr("FileList refresh is deferred while operation %1 is active")
                    .arg(operationCoordinator_.activeOperationId()));
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
