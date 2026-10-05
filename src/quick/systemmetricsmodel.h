#pragma once

#include "systemmonitor.h"

#include <QObject>
#include <QString>

class QTimer;

class SystemMetricsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool sampled READ sampled NOTIFY metricsChanged)
    Q_PROPERTY(QString cpuName READ cpuName CONSTANT)
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY metricsChanged)
    Q_PROPERTY(bool cpuUsageAvailable READ cpuUsageAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(double cpuTemperature READ cpuTemperature
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool cpuTemperatureAvailable
                   READ cpuTemperatureAvailable NOTIFY metricsChanged)
    Q_PROPERTY(double cpuFrequencyMHz READ cpuFrequencyMHz
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool cpuFrequencyAvailable
                   READ cpuFrequencyAvailable NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuPresent READ gpuPresent NOTIFY metricsChanged)
    Q_PROPERTY(QString gpuName READ gpuName NOTIFY metricsChanged)
    Q_PROPERTY(double gpuUsage READ gpuUsage NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuUsageAvailable READ gpuUsageAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(double gpuTemperature READ gpuTemperature
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuTemperatureAvailable
                   READ gpuTemperatureAvailable NOTIFY metricsChanged)
    Q_PROPERTY(double gpuFrequencyMHz READ gpuFrequencyMHz
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuFrequencyAvailable
                   READ gpuFrequencyAvailable NOTIFY metricsChanged)
    Q_PROPERTY(double gpuPowerWatts READ gpuPowerWatts
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuPowerAvailable READ gpuPowerAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(qint64 gpuVramUsedMB READ gpuVramUsedMB
                   NOTIFY metricsChanged)
    Q_PROPERTY(qint64 gpuVramTotalMB READ gpuVramTotalMB
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool gpuVramAvailable READ gpuVramAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(double ramUsage READ ramUsage NOTIFY metricsChanged)
    Q_PROPERTY(bool ramUsageAvailable READ ramUsageAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(qint64 ramUsedMB READ ramUsedMB NOTIFY metricsChanged)
    Q_PROPERTY(qint64 ramTotalMB READ ramTotalMB NOTIFY metricsChanged)
    Q_PROPERTY(double diskUsage READ diskUsage NOTIFY metricsChanged)
    Q_PROPERTY(bool diskUsageAvailable READ diskUsageAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(qint64 diskUsedGB READ diskUsedGB NOTIFY metricsChanged)
    Q_PROPERTY(qint64 diskTotalGB READ diskTotalGB NOTIFY metricsChanged)
    Q_PROPERTY(bool networkAvailable READ networkAvailable
                   NOTIFY metricsChanged)
    Q_PROPERTY(double rxSpeedKBs READ rxSpeedKBs
                   NOTIFY metricsChanged)
    Q_PROPERTY(double txSpeedKBs READ txSpeedKBs
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool dashboardActive READ dashboardActive
                   WRITE setDashboardActive
                   NOTIFY dashboardActiveChanged)

public:
    explicit SystemMetricsModel(QObject *parent = nullptr);

    bool sampled() const;
    QString cpuName() const;
    double cpuUsage() const;
    bool cpuUsageAvailable() const;
    double cpuTemperature() const;
    bool cpuTemperatureAvailable() const;
    double cpuFrequencyMHz() const;
    bool cpuFrequencyAvailable() const;

    bool gpuPresent() const;
    QString gpuName() const;
    double gpuUsage() const;
    bool gpuUsageAvailable() const;
    double gpuTemperature() const;
    bool gpuTemperatureAvailable() const;
    double gpuFrequencyMHz() const;
    bool gpuFrequencyAvailable() const;
    double gpuPowerWatts() const;
    bool gpuPowerAvailable() const;
    qint64 gpuVramUsedMB() const;
    qint64 gpuVramTotalMB() const;
    bool gpuVramAvailable() const;

    double ramUsage() const;
    bool ramUsageAvailable() const;
    qint64 ramUsedMB() const;
    qint64 ramTotalMB() const;

    double diskUsage() const;
    bool diskUsageAvailable() const;
    qint64 diskUsedGB() const;
    qint64 diskTotalGB() const;

    bool networkAvailable() const;
    double rxSpeedKBs() const;
    double txSpeedKBs() const;
    bool dashboardActive() const;

    Q_INVOKABLE void refresh();
    void setDashboardActive(bool active);

signals:
    void metricsChanged();
    void dashboardActiveChanged();

private:
    friend class QuickClientTests;

    explicit SystemMetricsModel(bool autoStart, QObject *parent);
    void applyMetrics(const SystemMetrics &metrics);
    const GpuMetrics *primaryGpu() const;

    SystemMonitor *monitor_;
    QTimer *updateTimer_;
    SystemMetrics metrics_;
    QString cpuName_;
    bool sampled_ = false;
    bool autoStart_ = false;
    bool dashboardActive_ = false;
};
