#include "systemmonitor.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QRegularExpression>
#include <QStorageInfo>
#include <algorithm>
#include <limits>

namespace {

QString normalizedPciHex(QString value) {
    value = value.trimmed();
    if (value.startsWith(QStringLiteral("0x"),
                         Qt::CaseInsensitive)) {
        value.remove(0, 2);
    }
    return value.toUpper();
}

}  // namespace

SystemMonitor::SystemMonitor(QObject *parent)
    : QObject(parent),
      nvidiaProvider_(new tryx::nvidia::NvidiaSmiProvider(this)) {
    cpuEnergyClock_.start();
    readCpuCoreCount();
    connect(nvidiaProvider_,
            &tryx::nvidia::NvidiaSmiProvider::snapshotChanged,
            this, &SystemMonitor::handleNvidiaSnapshotChanged,
            Qt::QueuedConnection);
}

void SystemMonitor::update() {
    metrics_.cpu.temperature =
        readCpuTemperature(&metrics_.cpu.temperatureAvailable);
    metrics_.cpu.usagePercent =
        readCpuUsage(&metrics_.cpu.usageAvailable);
    metrics_.cpu.frequencyMHz =
        readCpuFrequency(&metrics_.cpu.frequencyAvailable);
    metrics_.cpu.powerWatts =
        readCpuPower(&metrics_.cpu.powerAvailable);
    metrics_.cpu.coreCount = readCpuCoreCount();
    metrics_.gpus = readGpuMetrics();
    metrics_.ram = readRamMetrics();
    metrics_.ram.frequencyMHz =
        readMemoryFrequency(&metrics_.ram.frequencyAvailable);
    metrics_.net = readNetMetrics();
    metrics_.disk = readDiskMetrics();

    emit metricsUpdated(metrics_);
}

QString SystemMonitor::cpuModelName() {
    QFile file(QStringLiteral("/proc/cpuinfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return cpuModelNameFromContents(file.readAll());
}

QString SystemMonitor::cpuModelNameFromContents(
    const QByteArray &contents) {
    const QList<QByteArray> lines =
        contents.split('\n');
    const QStringList preferredKeys{
        QStringLiteral("model name"),
        QStringLiteral("Hardware"),
        QStringLiteral("Processor")};
    for (const QString &preferredKey : preferredKeys) {
        for (const QByteArray &rawLine : lines) {
            const QString line =
                QString::fromLocal8Bit(rawLine);
            const qsizetype separator =
                line.indexOf(QLatin1Char(':'));
            if (separator < 0 ||
                line.left(separator).trimmed().compare(
                    preferredKey,
                    Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString value =
                line.mid(separator + 1).trimmed();
            if (!value.isEmpty()) {
                return value;
            }
        }
    }
    return {};
}

QString SystemMonitor::primaryGpuModelName() {
    if (!metrics_.gpus.isEmpty()) {
        return metrics_.gpus.constFirst().name;
    }
    return primaryGpuModelNameFromDrmRoot(
        QStringLiteral("/sys/class/drm"));
}

QString SystemMonitor::primaryGpuModelNameFromDrmRoot(
    const QString &drmRoot) {
    const auto inventory = tryx::buildGpuInventory(
        scanGpuBaseRows(drmRoot), {}, 1);
    return inventory.ok && !inventory.gpus.isEmpty()
        ? inventory.gpus.constFirst().name
        : QString();
}

void SystemMonitor::setNvidiaSampleDemand(
    tryx::nvidia::NvidiaSampleDemand demand) {
    nvidiaDemand_ = demand;
    if (gpuTopologyInitialized_ && nvidiaProviderRequestsEnabled()) {
        nvidiaProvider_->requestSample(demand,
                                       gpuTopologyGeneration_);
    }
    if (demand == tryx::nvidia::NvidiaSampleDemand::Off &&
        invalidateCachedNvidiaTelemetry()) {
        emit metricsUpdated(metrics_);
    }
}

bool SystemMonitor::nvidiaProviderRequestsEnabled() const {
#ifdef TRYX_PROTOCOL_TESTING
    return nvidiaProviderRequestsEnabledForTesting_;
#else
    return true;
#endif
}

bool SystemMonitor::invalidateCachedNvidiaTelemetry() {
    bool changed = false;
    for (GpuMetrics &gpu : metrics_.gpus) {
        if (gpu.vendor != GpuVendor::Nvidia) {
            continue;
        }
        changed = changed || gpu.temperature != 0.0 ||
            gpu.usagePercent != 0.0 || gpu.frequencyMHz != 0.0 ||
            gpu.voltageMV != 0.0 || gpu.powerWatts != 0.0 ||
            gpu.vramUsedMB != 0 || gpu.vramTotalMB != 0 ||
            gpu.temperatureAvailable || gpu.usageAvailable ||
            gpu.frequencyAvailable || gpu.powerAvailable ||
            gpu.vramAvailable;
        gpu.temperature = 0.0;
        gpu.usagePercent = 0.0;
        gpu.frequencyMHz = 0.0;
        gpu.voltageMV = 0.0;
        gpu.powerWatts = 0.0;
        gpu.vramUsedMB = 0;
        gpu.vramTotalMB = 0;
        gpu.temperatureAvailable = false;
        gpu.usageAvailable = false;
        gpu.frequencyAvailable = false;
        gpu.powerAvailable = false;
        gpu.vramAvailable = false;
    }
    return changed;
}

QString SystemMonitor::readSysFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return file.readAll().trimmed();
}

QString SystemMonitor::findHwmonByName(const QString &name) {
    QDir hwmonDir("/sys/class/hwmon");
    for (const auto &entry : hwmonDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString namePath = hwmonDir.filePath(entry) + "/name";
        if (readSysFile(namePath) == name) {
            return hwmonDir.filePath(entry);
        }
    }
    return {};
}

double SystemMonitor::readCpuTemperature(bool *available) {
    *available = false;
    // AMD Ryzen: k10temp or zenpower
    QString hwmon = findHwmonByName("k10temp");
    if (hwmon.isEmpty()) {
        hwmon = findHwmonByName("zenpower");
    }
    if (hwmon.isEmpty()) {
        return 0.0;
    }

    // Tctl temperature
    QString val = readSysFile(hwmon + "/temp1_input");
    if (val.isEmpty()) {
        return 0.0;
    }
    bool ok = false;
    const double value = val.toDouble(&ok) / 1000.0;
    *available = ok;
    return ok ? value : 0.0;
}

double SystemMonitor::readCpuUsage(bool *available) {
    *available = false;
    QFile file("/proc/stat");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0.0;
    }

    QString line = file.readLine();
    QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() < 8 || parts[0] != "cpu") {
        return 0.0;
    }

    int64_t user = parts[1].toLongLong();
    int64_t nice = parts[2].toLongLong();
    int64_t system = parts[3].toLongLong();
    int64_t idle = parts[4].toLongLong();
    int64_t iowait = parts[5].toLongLong();
    int64_t irq = parts[6].toLongLong();
    int64_t softirq = parts[7].toLongLong();

    int64_t totalIdle = idle + iowait;
    int64_t total = user + nice + system + idle + iowait + irq + softirq;

    int64_t deltaIdle = totalIdle - prevCpuIdle_;
    int64_t deltaTotal = total - prevCpuTotal_;

    const bool hasPreviousSample = prevCpuTotal_ > 0;

    prevCpuIdle_ = totalIdle;
    prevCpuTotal_ = total;

    if (!hasPreviousSample || deltaTotal <= 0 || deltaIdle < 0 ||
        deltaIdle > deltaTotal) {
        return 0.0;
    }

    *available = true;
    return (1.0 - static_cast<double>(deltaIdle) / deltaTotal) * 100.0;
}

double SystemMonitor::readCpuFrequency(bool *available) {
    *available = false;
    // Average frequency across all cores
    QDir cpuDir("/sys/devices/system/cpu");
    double totalFreq = 0.0;
    int count = 0;

    for (const auto &entry : cpuDir.entryList(QStringList{"cpu[0-9]*"}, QDir::Dirs)) {
        QString freqPath = cpuDir.filePath(entry) + "/cpufreq/scaling_cur_freq";
        QString val = readSysFile(freqPath);
        if (!val.isEmpty()) {
            totalFreq += val.toDouble() / 1000.0; // kHz -> MHz
            count++;
        }
    }

    *available = count > 0;
    return count > 0 ? totalFreq / count : 0.0;
}

double SystemMonitor::readCpuPower(bool *available) {
    *available = false;
    QString hwmon = findHwmonByName(QStringLiteral("zenpower"));
    if (hwmon.isEmpty()) {
        hwmon = findHwmonByName(QStringLiteral("k10temp"));
    }
    if (!hwmon.isEmpty()) {
        const QString value = readSysFile(hwmon + QStringLiteral("/power1_average"));
        bool ok = false;
        const double microwatts = value.toDouble(&ok);
        if (ok && microwatts >= 0.0) {
            *available = true;
            return microwatts / 1000000.0;
        }
    }

    if (cpuEnergyPath_.isEmpty()) {
        QDir powercap(QStringLiteral("/sys/class/powercap"));
        const QStringList entries = powercap.entryList(
            QStringList{QStringLiteral("*rapl:*")},
            QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            const QString directory = powercap.filePath(entry);
            const QString name = readSysFile(directory + QStringLiteral("/name"));
            const QString energyPath =
                directory + QStringLiteral("/energy_uj");
            if (!name.startsWith(QStringLiteral("package")) ||
                readSysFile(energyPath).isEmpty()) {
                continue;
            }
            cpuEnergyPath_ = energyPath;
            bool maxOk = false;
            cpuMaxEnergyRangeUj_ = readSysFile(
                directory + QStringLiteral("/max_energy_range_uj"))
                                           .toLongLong(&maxOk);
            if (!maxOk) {
                cpuMaxEnergyRangeUj_ = 0;
            }
            break;
        }
    }
    if (cpuEnergyPath_.isEmpty()) {
        return 0.0;
    }

    bool energyOk = false;
    const qint64 energyUj = readSysFile(cpuEnergyPath_).toLongLong(&energyOk);
    const qint64 nowMs = cpuEnergyClock_.elapsed();
    if (!energyOk || energyUj < 0) {
        return 0.0;
    }
    const qint64 previousEnergy = prevCpuEnergyUj_;
    const qint64 previousTimestamp = prevCpuEnergyElapsedMs_;
    prevCpuEnergyUj_ = energyUj;
    prevCpuEnergyElapsedMs_ = nowMs;
    if (previousEnergy < 0 || previousTimestamp < 0) {
        return 0.0;
    }
    double powerWatts = 0.0;
    if (!calculateRaplPowerWatts(previousEnergy, energyUj,
                                 cpuMaxEnergyRangeUj_,
                                 nowMs - previousTimestamp,
                                 &powerWatts)) {
        return 0.0;
    }
    *available = true;
    return powerWatts;
}

bool SystemMonitor::calculateRaplPowerWatts(qint64 previousEnergyUj,
                                             qint64 currentEnergyUj,
                                             qint64 maxEnergyRangeUj,
                                             qint64 intervalMs,
                                             double *powerWatts) {
    constexpr qint64 kMaximumRaplSampleIntervalMs = 5000;
    if (!powerWatts || previousEnergyUj < 0 || currentEnergyUj < 0 ||
        intervalMs <= 0 || intervalMs > kMaximumRaplSampleIntervalMs) {
        return false;
    }
    qint64 deltaUj = currentEnergyUj - previousEnergyUj;
    if (deltaUj < 0 && maxEnergyRangeUj > previousEnergyUj) {
        deltaUj = maxEnergyRangeUj - previousEnergyUj + currentEnergyUj;
    }
    if (deltaUj < 0) {
        return false;
    }
    *powerWatts = static_cast<double>(deltaUj) /
                  static_cast<double>(intervalMs) / 1000.0;
    return true;
}

int SystemMonitor::readCpuCoreCount() {
    QDir cpuDir("/sys/devices/system/cpu");
    int count = cpuDir.entryList(QStringList{"cpu[0-9]*"}, QDir::Dirs).size();
    return count > 0 ? count : 1;
}

QVector<GpuMetrics> SystemMonitor::readGpuMetrics() {
    return readGpuMetricsFromDrmRoot(
        gpuDrmRoot_, nvidiaProviderRequestsEnabled());
}

QVector<GpuMetrics> SystemMonitor::readGpuMetricsFromDrmRoot(
    const QString &drmRoot, bool requestProvider) {
    const QVector<GpuMetrics> baseRows = scanGpuBaseRows(drmRoot);
    bool fingerprintOk = false;
    const QString fingerprint = tryx::gpuTopologyFingerprint(
        baseRows, &fingerprintOk);
    if (!fingerprintOk) {
        return {};
    }
    if (!gpuTopologyInitialized_ ||
        gpuTopologyFingerprint_ != fingerprint) {
        gpuTopologyInitialized_ = true;
        gpuTopologyFingerprint_ = fingerprint;
        ++gpuTopologyGeneration_;
        gpuProviderIdentityTracker_.reset();
    }

    const bool hasNvidia = std::any_of(
        baseRows.cbegin(), baseRows.cend(),
        [](const GpuMetrics &gpu) {
            return gpu.vendor == GpuVendor::Nvidia;
        });
    QVector<tryx::nvidia::GpuSample> providerRows;
    if (requestProvider) {
        nvidiaProvider_->requestSample(
            hasNvidia ? nvidiaDemand_
                      : tryx::nvidia::NvidiaSampleDemand::Off,
            gpuTopologyGeneration_);
        if (hasNvidia) {
            providerRows = nvidiaProvider_->snapshot();
        }
    }

    tryx::GpuInventoryResult inventory = tryx::buildGpuInventory(
        baseRows, providerRows, gpuTopologyGeneration_);
    if (inventory.ok && !providerRows.isEmpty() &&
        !gpuProviderIdentityTracker_.accept(providerRows)) {
        ++gpuTopologyGeneration_;
        gpuProviderIdentityTracker_.reset();
        nvidiaProvider_->requestSample(
            hasNvidia ? nvidiaDemand_
                      : tryx::nvidia::NvidiaSampleDemand::Off,
            gpuTopologyGeneration_);
        providerRows.clear();
        inventory = tryx::buildGpuInventory(
            baseRows, {}, gpuTopologyGeneration_);
    }
    if (!inventory.ok && !providerRows.isEmpty()) {
        inventory = tryx::buildGpuInventory(
            baseRows, {}, gpuTopologyGeneration_);
    }
    return inventory.ok ? inventory.gpus : QVector<GpuMetrics>{};
}

QVector<GpuMetrics> SystemMonitor::scanGpuBaseRows(
    const QString &drmRoot) {
    QVector<GpuMetrics> gpus;
    QDir drmDirectory(drmRoot);
    for (const QString &entry : drmDirectory.entryList(
             QStringList{QStringLiteral("card[0-9]*")}, QDir::Dirs)) {
        if (entry.contains(QLatin1Char('-'))) {
            continue;
        }
        const QString cardPath = drmDirectory.filePath(entry) +
                                 QStringLiteral("/device");
        const QString bdf = gpuPciBdf(cardPath);
        if (bdf.isEmpty()) {
            continue;
        }

        GpuMetrics gpu;
        gpu.pciBdf = bdf;
        gpu.pciDeviceId = normalizedPciHex(
            readSysFile(cardPath + QStringLiteral("/device")));
        gpu.pciRevisionId = normalizedPciHex(
            readSysFile(cardPath + QStringLiteral("/revision")));
        gpu.name = resolveGpuModelName(cardPath);
        gpu.bootVga = readSysFile(
            cardPath + QStringLiteral("/boot_vga")) ==
            QStringLiteral("1");
        const QString vendor = normalizedPciHex(
            readSysFile(cardPath + QStringLiteral("/vendor")));
        if (vendor == QStringLiteral("10DE")) {
            gpu.vendor = GpuVendor::Nvidia;
        } else if (vendor == QStringLiteral("1002")) {
            gpu.vendor = GpuVendor::Amd;
        } else if (vendor == QStringLiteral("8086")) {
            gpu.vendor = GpuVendor::Intel;
        }

        bool usageOk = false;
        const double usage = readSysFile(
            cardPath + QStringLiteral("/gpu_busy_percent"))
                                 .toDouble(&usageOk);
        if (usageOk && usage >= 0.0 && usage <= 100.0) {
            gpu.usagePercent = usage;
            gpu.usageAvailable = true;
        }

        QDir hwmonDirectory(cardPath + QStringLiteral("/hwmon"));
        for (const QString &hwmonEntry : hwmonDirectory.entryList(
                 QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString hwmonPath =
                hwmonDirectory.filePath(hwmonEntry);
            bool temperatureOk = false;
            const double temperature = readSysFile(
                hwmonPath + QStringLiteral("/temp1_input"))
                                           .toDouble(&temperatureOk) /
                                       1000.0;
            if (!gpu.temperatureAvailable && temperatureOk &&
                temperature >= 0.0 && temperature <= 255.0) {
                gpu.temperature = temperature;
                gpu.temperatureAvailable = true;
            }
            bool powerOk = false;
            const double power = readSysFile(
                hwmonPath + QStringLiteral("/power1_average"))
                                     .toDouble(&powerOk) /
                                 1000000.0;
            if (!gpu.powerAvailable && powerOk && power >= 0.0 &&
                power <= 10000.0) {
                gpu.powerWatts = power;
                gpu.powerAvailable = true;
            }
            bool voltageOk = false;
            const double voltage = readSysFile(
                hwmonPath + QStringLiteral("/in0_input"))
                                       .toDouble(&voltageOk);
            if (voltageOk && voltage >= 0.0) {
                gpu.voltageMV = voltage;
            }
        }

        const QString clockData = readSysFile(
            cardPath + QStringLiteral("/pp_dpm_sclk"));
        for (const QString &line : clockData.split(QLatin1Char('\n'))) {
            if (!line.contains(QLatin1Char('*'))) {
                continue;
            }
            static const QRegularExpression clockExpression(
                QStringLiteral("(\\d+)Mhz"));
            const auto match = clockExpression.match(line);
            bool frequencyOk = false;
            const double frequency = match.hasMatch()
                ? match.captured(1).toDouble(&frequencyOk)
                : 0.0;
            if (frequencyOk && frequency >= 0.0 &&
                frequency <= 100000.0) {
                gpu.frequencyMHz = frequency;
                gpu.frequencyAvailable = true;
            }
            break;
        }

        bool usedOk = false;
        bool totalOk = false;
        const quint64 usedBytes = readSysFile(
            cardPath + QStringLiteral("/mem_info_vram_used"))
                                      .toULongLong(&usedOk);
        const quint64 totalBytes = readSysFile(
            cardPath + QStringLiteral("/mem_info_vram_total"))
                                       .toULongLong(&totalOk);
        constexpr quint64 bytesPerMiB = 1024U * 1024U;
        if (usedOk && totalOk && totalBytes > 0 &&
            usedBytes <= totalBytes) {
            const quint64 usedMiB = usedBytes / bytesPerMiB;
            const quint64 totalMiB = totalBytes / bytesPerMiB;
            if (totalMiB > 0 &&
                totalMiB <= static_cast<quint64>(
                    std::numeric_limits<int64_t>::max())) {
                gpu.vramUsedMB = static_cast<int64_t>(usedMiB);
                gpu.vramTotalMB = static_cast<int64_t>(totalMiB);
                gpu.vramAvailable = true;
            }
        }
        gpus.append(gpu);
    }
    return gpus;
}

QString SystemMonitor::gpuPciBdf(const QString &cardPath) {
    const QString canonical = QFileInfo(cardPath).canonicalFilePath();
    const QString fromCanonical = tryx::normalizeGpuPciBdf(
        QFileInfo(canonical).fileName());
    if (!fromCanonical.isEmpty()) {
        return fromCanonical;
    }
    const QString uevent = readSysFile(
        cardPath + QStringLiteral("/uevent"));
    for (const QString &line : uevent.split(QLatin1Char('\n'))) {
        constexpr auto prefix = "PCI_SLOT_NAME=";
        if (line.startsWith(QString::fromLatin1(prefix))) {
            return tryx::normalizeGpuPciBdf(
                line.mid(sizeof("PCI_SLOT_NAME=") - 1));
        }
    }
    return {};
}

void SystemMonitor::handleNvidiaSnapshotChanged() {
    if (refreshingGpuMetrics_) {
        return;
    }
    refreshingGpuMetrics_ = true;
    metrics_.gpus = readGpuMetrics();
    refreshingGpuMetrics_ = false;
    emit metricsUpdated(metrics_);
}

QString SystemMonitor::resolveGpuModelName(
    const QString &cardPath) {
    const QString vendor =
        normalizedPciHex(readSysFile(cardPath + "/vendor"));
    const QString device =
        normalizedPciHex(readSysFile(cardPath + "/device"));
    const QString revision =
        normalizedPciHex(readSysFile(cardPath + "/revision"));
    const QString bdf = gpuPciBdf(cardPath);
    const QString cacheKey =
        (bdf.isEmpty() ? cardPath : bdf) + QLatin1Char('|') + vendor +
        QLatin1Char('|') + device + QLatin1Char('|') + revision;
    if (gpuModelCache_.contains(cacheKey)) {
        return gpuModelCache_.value(cacheKey);
    }

    QString name = readSysFile(cardPath + "/product_name");
    if (name.isEmpty() && vendor == QStringLiteral("1002")) {
        name = readAmdGpuMarketingName(cardPath);
    }
    if (name.isEmpty()) {
        name = readUdevPciModelName(cardPath);
    }
    gpuModelCache_.insert(cacheKey, name.trimmed());
    return name.trimmed();
}

QString SystemMonitor::readAmdGpuMarketingName(
    const QString &cardPath) {
    const QString device =
        normalizedPciHex(readSysFile(cardPath + "/device"));
    const QString revision =
        normalizedPciHex(readSysFile(cardPath + "/revision"));
    if (device.isEmpty() || revision.isEmpty()) {
        return {};
    }

    return readAmdGpuMarketingNameFromIdsFile(
        QStringLiteral("/usr/share/libdrm/amdgpu.ids"),
        device, revision);
}

QString SystemMonitor::readAmdGpuMarketingNameFromIdsFile(
    const QString &idsPath, const QString &device,
    const QString &revision) {
    const QString normalizedDevice =
        normalizedPciHex(device);
    const QString normalizedRevision =
        normalizedPciHex(revision);
    if (normalizedDevice.isEmpty() ||
        normalizedRevision.isEmpty()) {
        return {};
    }
    QFile ids(idsPath);
    if (!ids.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    while (!ids.atEnd()) {
        const QString line =
            QString::fromLocal8Bit(ids.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char(','));
        if (fields.size() < 3 ||
            normalizedPciHex(fields.at(0)) !=
                normalizedDevice ||
            normalizedPciHex(fields.at(1)) !=
                normalizedRevision) {
            continue;
        }
        return fields.mid(2).join(QLatin1Char(',')).trimmed();
    }
    return {};
}

QString SystemMonitor::readUdevPciModelName(
    const QString &cardPath) {
    const QString uevent = readSysFile(cardPath + "/uevent");
    QString slot;
    for (const QString &line : uevent.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("PCI_SLOT_NAME="))) {
            slot = line.mid(
                QStringLiteral("PCI_SLOT_NAME=").size()).trimmed();
            break;
        }
    }
    if (slot.isEmpty()) {
        return {};
    }
    QFile database(
        QStringLiteral("/run/udev/data/+pci:") + slot);
    if (!database.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    while (!database.atEnd()) {
        const QString line =
            QString::fromLocal8Bit(database.readLine()).trimmed();
        constexpr auto prefix = "E:ID_MODEL_FROM_DATABASE=";
        if (line.startsWith(QString::fromLatin1(prefix))) {
            return line.mid(
                QString::fromLatin1(prefix).size()).trimmed();
        }
    }
    return {};
}

RamMetrics SystemMonitor::readRamMetrics() {
    RamMetrics ram;

    QFile file("/proc/meminfo");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return ram;
    }

    QMap<QString, int64_t> info;
    QTextStream in(&file);
    QString line;
    while (!(line = in.readLine()).isNull()) {
        QStringList parts = line.split(QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            info[parts[0]] = parts[1].toLongLong();
        }
    }

    ram.totalMB = info.value("MemTotal", 0) / 1024;
    ram.availableMB = info.value("MemAvailable", 0) / 1024;
    ram.usedMB = ram.totalMB - ram.availableMB;
    ram.usagePercent = ram.totalMB > 0
                           ? static_cast<double>(ram.usedMB) / ram.totalMB * 100.0
                           : 0.0;
    ram.usageAvailable = ram.totalMB > 0 &&
                         info.contains(QStringLiteral("MemAvailable"));

    return ram;
}

double SystemMonitor::readMemoryFrequency(bool *available) {
    *available = false;
    // Upstream Linux does not expose a portable unprivileged current DRAM
    // clock. SMBIOS Type 17 is privileged, static and reports MT/s rather
    // than MHz, so presenting it as a live MHz sensor would be incorrect.
    return 0.0;
}

NetMetrics SystemMonitor::readNetMetrics() {
    NetMetrics net;

    QFile file("/proc/net/dev");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        prevRxBytes_ = 0;
        prevTxBytes_ = 0;
        prevNetTimestamp_ = 0;
        return net;
    }

    int64_t totalRx = 0;
    int64_t totalTx = 0;
    bool countersAvailable = false;

    QTextStream in(&file);
    QString line;
    while (!(line = in.readLine()).isNull()) {
        line = line.trimmed();
        if (!line.contains(':')) {
            continue;
        }

        QString iface = line.section(':', 0, 0).trimmed();
        if (iface == "lo") {
            continue;
        }

        QStringList parts = line.section(':', 1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 9) {
            bool rxOk = false;
            bool txOk = false;
            const int64_t rxBytes = parts[0].toLongLong(&rxOk);
            const int64_t txBytes = parts[8].toLongLong(&txOk);
            if (rxOk && txOk && rxBytes >= 0 && txBytes >= 0) {
                totalRx += rxBytes;
                totalTx += txBytes;
                countersAvailable = true;
            }
        }
    }

    if (!countersAvailable) {
        prevRxBytes_ = 0;
        prevTxBytes_ = 0;
        prevNetTimestamp_ = 0;
        return net;
    }

    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (prevNetTimestamp_ > 0 && now > prevNetTimestamp_ &&
        totalRx >= prevRxBytes_ && totalTx >= prevTxBytes_) {
        const double dtSec =
            static_cast<double>(now - prevNetTimestamp_) / 1000.0;
        if (dtSec > 0.0) {
            net.rxSpeedKBs = (totalRx - prevRxBytes_) / 1024.0 / dtSec;
            net.txSpeedKBs = (totalTx - prevTxBytes_) / 1024.0 / dtSec;
            net.available = true;
        }
    }

    prevRxBytes_ = totalRx;
    prevTxBytes_ = totalTx;
    prevNetTimestamp_ = now;

    return net;
}

DiskMetrics SystemMonitor::readDiskMetrics() {
    DiskMetrics disk;
    const QStorageInfo storage = QStorageInfo::root();
    const qint64 totalBytes = storage.bytesTotal();
    const qint64 availableBytes = storage.bytesAvailable();
    if (storage.isValid() && storage.isReady() &&
        totalBytes > 0 && availableBytes >= 0 &&
        availableBytes <= totalBytes) {
        constexpr qint64 bytesPerGiB = 1024LL * 1024 * 1024;
        const qint64 usedBytes = totalBytes - availableBytes;
        disk.totalGB = totalBytes / bytesPerGiB;
        disk.usedGB = usedBytes / bytesPerGiB;
        disk.usagePercent =
            static_cast<double>(usedBytes) /
            static_cast<double>(totalBytes) * 100.0;
        disk.usageAvailable = true;
    }
    disk.temperature = readDiskTemperature();
    return disk;
}

double SystemMonitor::readDiskTemperature() {
    double maxTemp = 0;
    QDir hwmonDir("/sys/class/hwmon");
    for (const auto &entry : hwmonDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString namePath = hwmonDir.filePath(entry) + "/name";
        if (readSysFile(namePath) == "nvme") {
            QString tempPath = hwmonDir.filePath(entry) + "/temp1_input";
            QString val = readSysFile(tempPath);
            if (!val.isEmpty()) {
                double temp = val.toDouble() / 1000.0;
                if (temp > maxTemp) maxTemp = temp;
            }
        }
    }
    return maxTemp;
}
