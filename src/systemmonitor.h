#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QVector>
#include <QMap>
#include <cstdint>

struct CpuMetrics {
    double temperature = 0.0;
    double usagePercent = 0.0;
    double frequencyMHz = 0.0;
    double powerWatts = 0.0;
    int coreCount = 0;
    bool temperatureAvailable = false;
    bool usageAvailable = false;
    bool frequencyAvailable = false;
    bool powerAvailable = false;
};

struct GpuMetrics {
    QString name;
    double temperature = 0.0;
    double usagePercent = 0.0;
    double frequencyMHz = 0.0;
    double voltageMV = 0.0;
    double powerWatts = 0.0;
    int64_t vramUsedMB = 0;
    int64_t vramTotalMB = 0;
    bool temperatureAvailable = false;
    bool usageAvailable = false;
    bool frequencyAvailable = false;
    bool powerAvailable = false;
};

struct RamMetrics {
    int64_t totalMB = 0;
    int64_t usedMB = 0;
    int64_t availableMB = 0;
    double usagePercent = 0.0;
    double frequencyMHz = 0.0;
    bool usageAvailable = false;
    bool frequencyAvailable = false;
};

struct NetMetrics {
    double rxSpeedKBs = 0.0;
    double txSpeedKBs = 0.0;
    bool available = false;
};

struct DiskMetrics {
    int64_t totalGB = 0;
    int64_t usedGB = 0;
    double usagePercent = 0.0;
    double temperature = 0.0;
    bool usageAvailable = false;
};

struct SystemMetrics {
    CpuMetrics cpu;
    QVector<GpuMetrics> gpus;
    RamMetrics ram;
    NetMetrics net;
    DiskMetrics disk;
};

class SystemMonitor : public QObject {
    Q_OBJECT
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
public:
    explicit SystemMonitor(QObject *parent = nullptr);

    SystemMetrics currentMetrics() const { return metrics_; }
    static QString cpuModelName();
    QString primaryGpuModelName();

public slots:
    void update();

signals:
    void metricsUpdated(const SystemMetrics &metrics);

private:
    double readCpuTemperature(bool *available);
    double readCpuUsage(bool *available);
    double readCpuFrequency(bool *available);
    double readCpuPower(bool *available);
    static bool calculateRaplPowerWatts(qint64 previousEnergyUj,
                                        qint64 currentEnergyUj,
                                        qint64 maxEnergyRangeUj,
                                        qint64 intervalMs,
                                        double *powerWatts);
    int readCpuCoreCount();
    QVector<GpuMetrics> readGpuMetrics();
    QString primaryGpuModelNameFromDrmRoot(
        const QString &drmRoot);
    QString resolveGpuModelName(const QString &cardPath);
    QString readAmdGpuMarketingName(const QString &cardPath);
    static QString readAmdGpuMarketingNameFromIdsFile(
        const QString &idsPath, const QString &device,
        const QString &revision);
    QString readUdevPciModelName(const QString &cardPath);
    RamMetrics readRamMetrics();
    double readMemoryFrequency(bool *available);
    NetMetrics readNetMetrics();
    DiskMetrics readDiskMetrics();
    double readDiskTemperature();

    QString readSysFile(const QString &path);
    QString findHwmonByName(const QString &name);
    static QString cpuModelNameFromContents(
        const QByteArray &contents);

    SystemMetrics metrics_;

    // For CPU usage delta calculation
    int64_t prevCpuIdle_ = 0;
    int64_t prevCpuTotal_ = 0;
    qint64 prevCpuEnergyUj_ = -1;
    qint64 prevCpuEnergyElapsedMs_ = -1;
    QElapsedTimer cpuEnergyClock_;
    QString cpuEnergyPath_;
    qint64 cpuMaxEnergyRangeUj_ = 0;
    QMap<QString, QString> gpuModelCache_;

    // For network speed delta calculation
    int64_t prevRxBytes_ = 0;
    int64_t prevTxBytes_ = 0;
    int64_t prevNetTimestamp_ = 0;
};
