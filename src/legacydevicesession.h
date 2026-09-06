#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <panorama/device.hpp>

class DeviceWorker;
class SystemMonitor;

// Owned by the worker; borrows the single telemetry provider.
class LegacyDeviceSession final : public QObject {
    Q_OBJECT
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
public:
    LegacyDeviceSession(DeviceWorker &events, SystemMonitor &metrics);
    ~LegacyDeviceSession() override;
    void quiesce();

    void connectDevice(const QString &port);
    void disconnectDevice();
    void doHandshake();
    void setBrightness(int value);
    void setScreenConfig(const QStringList &media, const QString &ratio,
                         const QString &screenMode, const QString &playMode,
                         const QStringList &sysinfoLabels,
                         const QString &settingsPosition,
                         const QString &settingsColor,
                         const QString &settingsAlign,
                         const QStringList &settingsBadges,
                         int filterOpacity,
                         const QString &presetId = QString(),
                         const QStringList &sysinfoLabels2 = {},
                         const QStringList &settingsBadges2 = {},
                         bool waterfallMode = false);
    void setRotation(int degrees);
    void rebootDevice();
    void deleteMedia(const QStringList &files);
    void uploadMedia(const QString &localPath);
    void refreshMediaList();
    void sendKeepalive();
    void sendSysinfo(const QStringList &labels, const QStringList &values,
                     const QStringList &units);
    void sendLegacyMetrics();

private:
    DeviceWorker &events_;
    SystemMonitor &metrics_;
    std::unique_ptr<panorama::Device> device_;
    QTimer *legacyMetricsTimer_;
};
