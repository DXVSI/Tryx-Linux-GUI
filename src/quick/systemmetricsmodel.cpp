#include "systemmetricsmodel.h"

#include <QTimer>

namespace {

constexpr int kMetricsUpdateIntervalMs = 2000;

}  // namespace

SystemMetricsModel::SystemMetricsModel(QObject *parent)
    : SystemMetricsModel(true, parent) {}

SystemMetricsModel::SystemMetricsModel(bool autoStart, QObject *parent)
    : QObject(parent),
      monitor_(new SystemMonitor(this)),
      updateTimer_(new QTimer(this)),
      cpuName_(SystemMonitor::cpuModelName().trimmed()) {
    updateTimer_->setInterval(kMetricsUpdateIntervalMs);
    connect(updateTimer_, &QTimer::timeout,
            monitor_, &SystemMonitor::update);
    connect(monitor_, &SystemMonitor::metricsUpdated,
            this, &SystemMetricsModel::applyMetrics);
    if (autoStart) {
        updateTimer_->start();
        QTimer::singleShot(0, monitor_, &SystemMonitor::update);
    }
}

bool SystemMetricsModel::sampled() const {
    return sampled_;
}

QString SystemMetricsModel::cpuName() const {
    return cpuName_;
}

double SystemMetricsModel::cpuUsage() const {
    return metrics_.cpu.usagePercent;
}

bool SystemMetricsModel::cpuUsageAvailable() const {
    return metrics_.cpu.usageAvailable;
}

double SystemMetricsModel::cpuTemperature() const {
    return metrics_.cpu.temperature;
}

bool SystemMetricsModel::cpuTemperatureAvailable() const {
    return metrics_.cpu.temperatureAvailable;
}

double SystemMetricsModel::cpuFrequencyMHz() const {
    return metrics_.cpu.frequencyMHz;
}

bool SystemMetricsModel::cpuFrequencyAvailable() const {
    return metrics_.cpu.frequencyAvailable;
}

bool SystemMetricsModel::gpuPresent() const {
    return primaryGpu() != nullptr;
}

QString SystemMetricsModel::gpuName() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->name : QString();
}

double SystemMetricsModel::gpuUsage() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->usagePercent : 0.0;
}

bool SystemMetricsModel::gpuUsageAvailable() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu && gpu->usageAvailable;
}

double SystemMetricsModel::gpuTemperature() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->temperature : 0.0;
}

bool SystemMetricsModel::gpuTemperatureAvailable() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu && gpu->temperatureAvailable;
}

double SystemMetricsModel::gpuFrequencyMHz() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->frequencyMHz : 0.0;
}

bool SystemMetricsModel::gpuFrequencyAvailable() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu && gpu->frequencyAvailable;
}

qint64 SystemMetricsModel::gpuVramUsedMB() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->vramUsedMB : 0;
}

qint64 SystemMetricsModel::gpuVramTotalMB() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu ? gpu->vramTotalMB : 0;
}

bool SystemMetricsModel::gpuVramAvailable() const {
    const GpuMetrics *gpu = primaryGpu();
    return gpu && gpu->vramTotalMB > 0 &&
           gpu->vramUsedMB >= 0;
}

double SystemMetricsModel::ramUsage() const {
    return metrics_.ram.usagePercent;
}

bool SystemMetricsModel::ramUsageAvailable() const {
    return metrics_.ram.usageAvailable;
}

qint64 SystemMetricsModel::ramUsedMB() const {
    return metrics_.ram.usedMB;
}

qint64 SystemMetricsModel::ramTotalMB() const {
    return metrics_.ram.totalMB;
}

double SystemMetricsModel::diskUsage() const {
    return metrics_.disk.usagePercent;
}

bool SystemMetricsModel::diskUsageAvailable() const {
    return metrics_.disk.usageAvailable;
}

qint64 SystemMetricsModel::diskUsedGB() const {
    return metrics_.disk.usedGB;
}

qint64 SystemMetricsModel::diskTotalGB() const {
    return metrics_.disk.totalGB;
}

bool SystemMetricsModel::networkAvailable() const {
    return metrics_.net.available;
}

double SystemMetricsModel::rxSpeedKBs() const {
    return metrics_.net.rxSpeedKBs;
}

double SystemMetricsModel::txSpeedKBs() const {
    return metrics_.net.txSpeedKBs;
}

void SystemMetricsModel::refresh() {
    monitor_->update();
}

void SystemMetricsModel::applyMetrics(
    const SystemMetrics &metrics) {
    metrics_ = metrics;
    sampled_ = true;
    emit metricsChanged();
}

const GpuMetrics *SystemMetricsModel::primaryGpu() const {
    return metrics_.gpus.isEmpty()
        ? nullptr
        : &metrics_.gpus.constFirst();
}
