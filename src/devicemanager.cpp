#include "devicemanager.h"
#include <QFileInfo>
#include <fstream>
#include <filesystem>

// --- DeviceWorker ---

DeviceWorker::DeviceWorker(QObject *parent)
    : QObject(parent) {}

DeviceWorker::~DeviceWorker() {
    if (device_ && device_->is_connected()) {
        device_->disconnect();
    }
}

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
    if (device_) {
        device_->disconnect();
        device_.reset();
    }
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
            emit error(tr("ffmpeg not found. Install: sudo dnf install ffmpeg"));
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
    : QObject(parent),
      worker_(new DeviceWorker),
      keepaliveTimer_(new QTimer(this)) {

    worker_->moveToThread(&workerThread_);

    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);

    // Forward signals from worker
    connect(worker_, &DeviceWorker::connected, this,
            [this](const QString &pid, const QString &serial,
                   const QString &fw, const QString &app) {
                connected_ = true;
                emit deviceConnected(pid, serial, fw, app);
            });
    connect(worker_, &DeviceWorker::disconnected, this,
            [this]() {
                connected_ = false;
                emit deviceDisconnected();
            });
    connect(worker_, &DeviceWorker::error, this, &DeviceManager::deviceError);
    connect(worker_, &DeviceWorker::brightnessSet, this, &DeviceManager::brightnessChanged);
    connect(worker_, &DeviceWorker::screenConfigSet, this, &DeviceManager::screenConfigChanged);
    connect(worker_, &DeviceWorker::mediaUploaded, this, &DeviceManager::mediaUploaded);
    connect(worker_, &DeviceWorker::mediaDeleted, this, &DeviceManager::mediaDeleted);
    connect(worker_, &DeviceWorker::mediaListReady, this, &DeviceManager::mediaListUpdated);
    connect(worker_, &DeviceWorker::uploadProgress, this, &DeviceManager::uploadStatus);

    // Connect internal signals to worker slots
    connect(this, &DeviceManager::requestConnect, worker_, &DeviceWorker::connectDevice);
    connect(this, &DeviceManager::requestDisconnect, worker_, &DeviceWorker::disconnectDevice);
    connect(this, &DeviceManager::requestBrightness, worker_, &DeviceWorker::setBrightness);
    connect(this, &DeviceManager::requestScreenConfig, worker_, &DeviceWorker::setScreenConfig);
    connect(this, &DeviceManager::requestDeleteMedia, worker_, &DeviceWorker::deleteMedia);
    connect(this, &DeviceManager::requestUploadMedia, worker_, &DeviceWorker::uploadMedia);
    connect(this, &DeviceManager::requestRefreshMedia, worker_, &DeviceWorker::refreshMediaList);
    connect(this, &DeviceManager::requestKeepalive, worker_, &DeviceWorker::sendKeepalive);
    connect(this, &DeviceManager::requestRotation, worker_, &DeviceWorker::setRotation);
    connect(this, &DeviceManager::requestReboot, worker_, &DeviceWorker::rebootDevice);
    connect(this, &DeviceManager::requestSysinfo, worker_, &DeviceWorker::sendSysinfo);
    connect(worker_, &DeviceWorker::sysinfoSent, this, &DeviceManager::sysinfoSent);

    // Keepalive timer
    connect(keepaliveTimer_, &QTimer::timeout, this,
            [this]() { emit requestKeepalive(); });

    workerThread_.start();
}

DeviceManager::~DeviceManager() {
    stopKeepalive();
    emit requestDisconnect();
    workerThread_.quit();
    workerThread_.wait();
}

void DeviceManager::connectDevice(const QString &port) {
    emit requestConnect(port);
}

void DeviceManager::disconnectDevice() {
    stopKeepalive();
    emit requestDisconnect();
}

void DeviceManager::setBrightness(int value) {
    emit requestBrightness(qBound(0, value, 100));
}

void DeviceManager::setScreenConfig(const QStringList &media, const QString &ratio,
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
    emit requestScreenConfig(media, ratio, screenMode, playMode,
                             sysinfoLabels, settingsPosition, settingsColor,
                             settingsAlign, settingsBadges, filterOpacity,
                             presetId, sysinfoLabels2, settingsBadges2,
                             waterfallMode);
}

void DeviceManager::sendSysinfo(const QStringList &labels, const QStringList &values,
                                const QStringList &units) {
    emit requestSysinfo(labels, values, units);
}

void DeviceManager::setRotation(int degrees) {
    emit requestRotation(degrees);
}

void DeviceManager::rebootDevice() {
    emit requestReboot();
}

void DeviceManager::deleteMedia(const QStringList &files) {
    emit requestDeleteMedia(files);
}

void DeviceManager::uploadMedia(const QString &localPath) {
    emit requestUploadMedia(localPath);
}

void DeviceManager::refreshMediaList() {
    emit requestRefreshMedia();
}

void DeviceManager::startKeepalive(int intervalSec) {
    keepaliveTimer_->start(intervalSec * 1000);
}

void DeviceManager::stopKeepalive() {
    keepaliveTimer_->stop();
}
