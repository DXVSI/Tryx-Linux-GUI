#include "deviceworker.h"
#include <panorama/adb.hpp>
#include <panorama/config.hpp>
#include <panorama/media.hpp>
#include "printerlifecycle_p.h"
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

namespace {

using namespace tryx::printer_lifecycle;

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
