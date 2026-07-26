#include "settingspage.h"
#include "devicemanager.h"
#include "firmwareupdater.h"
#include "printerprotocol.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QMessageBox>
#include <QDir>
#include <QProcess>
#include <QFileDialog>
#include <QFileInfo>

#include <panorama/config.hpp>

SettingsPage::SettingsPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr),
      firmwareUpdater_(new FirmwareUpdater(this)),
      autostartProcess_(new QProcess(this)) {
    setupUi();
    loadSettings();

    connect(deviceMgr_, &DeviceManager::printerDeviceInfoReady, this,
            [this](const PrinterProtocol::DeviceInfo &info) {
                deviceInfoBtn_->setEnabled(true);
                const QString message =
                    tr("Transport: USB printer-class\n"
                       "Device: %1\n"
                       "USB product: %2 %3\n"
                       "USB serial: %4\n"
                       "Product: %5\n"
                       "Serial: %6\n"
                       "Chip ID: %7\n"
                       "OS: %8 %9\n"
                       "Firmware: %10\n"
                       "App version: %11\n"
                       "Serial locked: %12")
                        .arg(info.devicePath,
                             info.manufacturer,
                             info.usbProduct,
                             info.usbSerial,
                             info.productName,
                             info.serialNumber,
                             info.chipId,
                             info.osName,
                             info.osVersion,
                             info.firmwareVersion,
                             info.appVersion,
                             info.serialNumberLocked ? tr("yes") : tr("no"));
                QMessageBox::information(this, tr("Device information"), message);
                emit statusMessage(tr("Printer-class device information loaded"));
            });
    connect(deviceMgr_, &DeviceManager::printerDeviceInfoFailed, this,
            [this](const QString &message) {
                deviceInfoBtn_->setEnabled(true);
                QMessageBox::information(this, tr("Device"), message);
            });
    autostartProcess_->setProcessChannelMode(QProcess::MergedChannels);
    connect(autostartProcess_, qOverload<int, QProcess::ExitStatus>(
                                   &QProcess::finished),
            this, &SettingsPage::onAutostartCommandFinished);
    connect(autostartProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart &&
                    autostartOperation_ != AutostartOperation::None) {
                    onAutostartCommandFinished(-1,
                                               QProcess::CrashExit);
                }
            });
    queryAutostartState();
}

void SettingsPage::setupUi() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Port settings
    connectionGroup_ = new QGroupBox(tr("Connection"));
    auto *portLayout = new QGridLayout(connectionGroup_);

    portCombo_ = new QComboBox;
    portCombo_->setEditable(true);
    portCombo_->addItem(tr("Auto"));
    refreshPortsBtn_ = new QPushButton(tr("Refresh"));

    portLayout->addWidget(new QLabel(tr("Port:")), 0, 0);
    portLayout->addWidget(portCombo_, 0, 1);
    portLayout->addWidget(refreshPortsBtn_, 0, 2);

    keepaliveSpin_ = new QSpinBox;
    keepaliveSpin_->setRange(5, 60);
    keepaliveSpin_->setValue(10);
    keepaliveSpin_->setSuffix(tr(" sec"));

    portLayout->addWidget(new QLabel(tr("Keepalive interval:")), 1, 0);
    portLayout->addWidget(keepaliveSpin_, 1, 1);

    mainLayout->addWidget(connectionGroup_);
    connectionGroup_->setVisible(
        !deviceMgr_->isPrinterClassDevicePresent());

    connect(refreshPortsBtn_, &QPushButton::clicked, this, &SettingsPage::onRefreshPorts);
    connect(deviceMgr_, &DeviceManager::printerPresenceChanged, this,
            [this](bool present) {
                connectionGroup_->setVisible(!present);
            });

    // Behavior
    auto *behaviorGroup = new QGroupBox(tr("Behavior"));
    auto *behaviorLayout = new QVBoxLayout(behaviorGroup);

    cbMinimizeToTray_ = new QCheckBox(tr("Minimize to tray on close"));
    cbStartMinimized_ = new QCheckBox(tr("Start minimized"));
    cbAutostart_ = new QCheckBox(tr("Autostart on login (systemd user service)"));

    cbMinimizeToTray_->setChecked(true);

    behaviorLayout->addWidget(cbMinimizeToTray_);
    behaviorLayout->addWidget(cbStartMinimized_);
    behaviorLayout->addWidget(cbAutostart_);

    mainLayout->addWidget(behaviorGroup);

    // Device info
    auto *infoGroup = new QGroupBox(tr("Device"));
    auto *infoLayout = new QHBoxLayout(infoGroup);

    deviceInfoBtn_ = new QPushButton(tr("Device information"));
    infoLayout->addWidget(deviceInfoBtn_);
    infoLayout->addStretch();

    mainLayout->addWidget(infoGroup);

    connect(deviceInfoBtn_, &QPushButton::clicked, this, &SettingsPage::onShowDeviceInfo);

    // Firmware update
    auto *firmwareGroup = new QGroupBox(tr("Firmware"));
    auto *firmwareLayout = new QGridLayout(firmwareGroup);

    firmwarePackageLabel_ = new QLabel(tr("No package selected"));
    firmwarePackageLabel_->setWordWrap(true);
    firmwarePackageLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    selectFirmwareBtn_ = new QPushButton(tr("Select firmware ZIP..."));
    validateFirmwareBtn_ = new QPushButton(tr("Validate"));
    flashFirmwareBtn_ = new QPushButton(tr("Flash firmware"));
    validateFirmwareBtn_->setEnabled(false);
    flashFirmwareBtn_->setEnabled(false);

    firmwareProgress_ = new QProgressBar;
    firmwareProgress_->setRange(0, 100);
    firmwareProgress_->setValue(0);

    firmwareStatusLabel_ = new QLabel(tr("Select a TRYX firmware package to enable flashing."));
    firmwareStatusLabel_->setWordWrap(true);

    firmwareLayout->addWidget(new QLabel(tr("Package:")), 0, 0);
    firmwareLayout->addWidget(firmwarePackageLabel_, 0, 1, 1, 3);
    firmwareLayout->addWidget(selectFirmwareBtn_, 1, 1);
    firmwareLayout->addWidget(validateFirmwareBtn_, 1, 2);
    firmwareLayout->addWidget(flashFirmwareBtn_, 1, 3);
    firmwareLayout->addWidget(firmwareProgress_, 2, 1, 1, 3);
    firmwareLayout->addWidget(firmwareStatusLabel_, 3, 1, 1, 3);
    firmwareLayout->setColumnStretch(1, 1);

    mainLayout->addWidget(firmwareGroup);

    connect(selectFirmwareBtn_, &QPushButton::clicked,
            this, &SettingsPage::onSelectFirmwarePackage);
    connect(validateFirmwareBtn_, &QPushButton::clicked,
            this, &SettingsPage::onValidateFirmwarePackage);
    connect(flashFirmwareBtn_, &QPushButton::clicked,
            this, &SettingsPage::onFlashFirmware);
    connect(firmwareUpdater_, &FirmwareUpdater::statusChanged,
            this, &SettingsPage::onFirmwareStatusChanged);
    connect(firmwareUpdater_, &FirmwareUpdater::progressChanged,
            this, &SettingsPage::onFirmwareProgressChanged);
    connect(firmwareUpdater_, &FirmwareUpdater::finished,
            this, &SettingsPage::onFirmwareFinished);
    updateFirmwareControls();

    // Buttons
    auto *btnLayout = new QHBoxLayout;
    saveBtn_ = new QPushButton(tr("Save"));
    resetBtn_ = new QPushButton(tr("Reset"));
    btnLayout->addStretch();
    btnLayout->addWidget(saveBtn_);
    btnLayout->addWidget(resetBtn_);

    mainLayout->addLayout(btnLayout);
    mainLayout->addStretch();

    connect(saveBtn_, &QPushButton::clicked, this, &SettingsPage::onSaveSettings);
    connect(resetBtn_, &QPushButton::clicked, this, &SettingsPage::onResetSettings);

    // Initial port scan
    onRefreshPorts();
}

void SettingsPage::loadSettings() {
    auto config = panorama::ConfigManager::load_config();
    if (config) {
        if (!config->port.empty()) {
            portCombo_->setCurrentText(QString::fromStdString(config->port));
        }
        keepaliveSpin_->setValue(config->keepalive_interval);
    }
}

QString SettingsPage::selectedPort() const {
    if (portCombo_->currentText() == tr("Auto")) {
        return {};
    }
    return portCombo_->currentText();
}

int SettingsPage::keepaliveInterval() const {
    return keepaliveSpin_->value();
}

bool SettingsPage::minimizeToTray() const {
    return cbMinimizeToTray_->isChecked();
}

bool SettingsPage::startMinimized() const {
    return cbStartMinimized_->isChecked();
}

void SettingsPage::onRefreshPorts() {
    QString current = portCombo_->currentText();
    portCombo_->clear();
    portCombo_->addItem(tr("Auto"));

    QDir devDir("/dev");
    for (const auto &entry : devDir.entryList(QStringList{"ttyACM*"}, QDir::System)) {
        portCombo_->addItem("/dev/" + entry);
    }

    int idx = portCombo_->findText(current);
    if (idx >= 0) {
        portCombo_->setCurrentIndex(idx);
    }
}

void SettingsPage::onShowDeviceInfo() {
    if (!deviceMgr_->isPrinterClassDevicePresent()) {
        if (!deviceMgr_->isConnected()) {
            QMessageBox::information(this, tr("Device"),
                                     tr("TRYX device is not connected"));
            return;
        }
        emit statusMessage(tr("Requesting device information..."));
        return;
    }

    deviceInfoBtn_->setEnabled(false);
    emit statusMessage(tr("Requesting printer-class device information..."));
    deviceMgr_->requestDeviceInfo();
}

void SettingsPage::onSelectFirmwarePackage() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        tr("Select firmware package"),
        QDir::homePath(),
        tr("Firmware packages (*.zip);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }

    firmwarePackagePath_ = selected;
    firmwarePackageValidated_ = false;
    firmwarePackageFlashSupported_ = false;
    firmwarePackageNeedsRockchipFlasher_ = false;
    const QFileInfo info(selected);
    firmwarePackageLabel_->setText(info.fileName() + "\n" + selected);
    firmwareStatusLabel_->setText(tr("Package selected. Validate it before flashing."));
    firmwareProgress_->setValue(0);
    updateFirmwareControls();
}

void SettingsPage::onValidateFirmwarePackage() {
    const QString dependencyMessage = firmwareDependencyMessage(false);
    if (!dependencyMessage.isEmpty()) {
        firmwareStatusLabel_->setText(dependencyMessage);
        QMessageBox::critical(this, tr("Firmware"), dependencyMessage);
        updateFirmwareControls();
        return;
    }

    const auto info = firmwareUpdater_->validatePackage(firmwarePackagePath_);
    if (!info.valid) {
        firmwarePackageValidated_ = false;
        firmwarePackageFlashSupported_ = false;
        firmwarePackageNeedsRockchipFlasher_ = false;
        flashFirmwareBtn_->setEnabled(false);
        firmwareStatusLabel_->setText(info.error);
        QMessageBox::critical(this, tr("Firmware"), info.error);
        return;
    }

    firmwarePackageValidated_ = true;
    firmwarePackageNeedsRockchipFlasher_ = info.kind == FirmwareUpdater::PackageKind::RockchipBundle;
    firmwarePackageFlashSupported_ =
        info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta ||
        (info.kind == FirmwareUpdater::PackageKind::RockchipBundle &&
         info.productCode == "PASE" &&
         firmwareUpdater_->rockchipFlashingAvailable());
    updateFirmwareControls();

    QString message;
    if (info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta) {
        message = tr("Type: Android OTA\nTarget: %1\nBuild: %2\nSize: %3 MiB")
                      .arg(info.preDevice,
                           info.postBuildIncremental,
                           QString::number(info.sizeBytes / 1024.0 / 1024.0, 'f', 1));
    } else {
        message = tr("Type: Rockchip loader bundle\nProduct: %1\nApp version: %2\nFirmware: %3\nMachine: %4\nPartitions: %5\nSize: %6 MiB")
                      .arg(info.productCode,
                           info.appVersion.isEmpty() ? tr("unknown") : info.appVersion,
                           info.firmwareVersion,
                           info.machineModel,
                           info.partitions.join(", "),
                           QString::number(info.sizeBytes / 1024.0 / 1024.0, 'f', 1));
        message += "\n\n" + firmwareUpdater_->rockchipFlashingUnavailableMessage();
    }
    firmwareStatusLabel_->setText(firmwarePackageFlashSupported_
                                      ? tr("Firmware package is valid")
                                      : tr("Firmware package is valid, but flashing is unavailable for this package."));
    QMessageBox::information(this, tr("Firmware package"), message);
}

void SettingsPage::onFlashFirmware() {
    const QString validationDependencyMessage = firmwareDependencyMessage(false);
    if (!validationDependencyMessage.isEmpty()) {
        firmwareStatusLabel_->setText(validationDependencyMessage);
        QMessageBox::critical(this, tr("Firmware"), validationDependencyMessage);
        updateFirmwareControls();
        return;
    }

    const auto info = firmwareUpdater_->validatePackage(firmwarePackagePath_);
    if (!info.valid) {
        firmwarePackageValidated_ = false;
        firmwarePackageFlashSupported_ = false;
        firmwarePackageNeedsRockchipFlasher_ = false;
        firmwareStatusLabel_->setText(info.error);
        QMessageBox::critical(this, tr("Firmware"), info.error);
        return;
    }

    firmwarePackageValidated_ = true;
    firmwarePackageNeedsRockchipFlasher_ = info.kind == FirmwareUpdater::PackageKind::RockchipBundle;
    firmwarePackageFlashSupported_ =
        info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta ||
        (info.kind == FirmwareUpdater::PackageKind::RockchipBundle &&
         info.productCode == "PASE" &&
         firmwareUpdater_->rockchipFlashingAvailable());

    const auto dependencies = firmwareUpdater_->dependencyStatus();
    if (info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta &&
        !dependencies.canFlashLegacy()) {
        QStringList missing;
        if (dependencies.adbPath.isEmpty()) {
            missing.append("adb");
        }
        if (dependencies.unzipPath.isEmpty()) {
            missing.append("unzip");
        }
        const QString dependencyMessage = tr("Missing firmware dependencies: %1")
                                              .arg(missing.join(", "));
        firmwareStatusLabel_->setText(dependencyMessage);
        QMessageBox::critical(this, tr("Firmware"), dependencyMessage);
        updateFirmwareControls();
        return;
    }

    if (info.kind == FirmwareUpdater::PackageKind::RockchipBundle &&
        info.productCode != "PASE") {
        const QString error = tr("Rockchip bundle product %1 is not supported by this Panorama SE updater")
                                  .arg(info.productCode);
        firmwareStatusLabel_->setText(error);
        QMessageBox::critical(this, tr("Firmware"), error);
        return;
    }
    if (info.kind == FirmwareUpdater::PackageKind::RockchipBundle) {
        const QString dependencyMessage = firmwareDependencyMessage(true);
        if (!dependencyMessage.isEmpty()) {
            firmwareStatusLabel_->setText(dependencyMessage);
            QMessageBox::critical(this, tr("Firmware"), dependencyMessage);
            updateFirmwareControls();
            return;
        }
        if (!firmwareUpdater_->rockchipFlashingAvailable()) {
            const QString error = firmwareUpdater_->rockchipFlashingUnavailableMessage();
            firmwareStatusLabel_->setText(error);
            QMessageBox::critical(this, tr("Firmware"), error);
            updateFirmwareControls();
            return;
        }
    }

    QString message;
    if (info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta) {
        message =
            tr("Package: %1\nType: Android OTA\nTarget: %2\nBuild: %3\n\n"
               "The package will be copied to the cooler and the device will reboot into recovery. "
               "Do not disconnect USB or power until the cooler finishes updating.")
                .arg(QFileInfo(info.path).fileName(), info.preDevice, info.postBuildIncremental);
    } else {
        message =
            tr("Package: %1\nType: Rockchip loader bundle\nProduct: %2\nApp version: %3\nFirmware: %4\n\n"
               "If the cooler is visible over ADB, it will reboot into Rockchip Loader mode. If it is already in Loader or Maskrom, the app will continue directly. The app will use external upgrade_tool to rewrite GPT, boot, recovery, rootfs, oem, and userdata. "
               "After this update the device may appear as RK PASE USB printer-class instead of ADB. "
               "Use this only for Panorama SE / PASE firmware. Do not disconnect USB or power until flashing finishes.")
                .arg(QFileInfo(info.path).fileName(),
                     info.productCode,
                     info.appVersion.isEmpty() ? tr("unknown") : info.appVersion,
                     info.firmwareVersion);
    }

    const auto choice = QMessageBox::warning(
        this,
        tr("Flash firmware?"),
        message,
        QMessageBox::Cancel | QMessageBox::Ok,
        QMessageBox::Cancel);
    if (choice != QMessageBox::Ok) {
        return;
    }

    setFirmwareBusy(true);
    firmwareProgress_->setValue(0);
    if (info.kind == FirmwareUpdater::PackageKind::LegacyAndroidOta) {
        firmwareUpdater_->startLegacyAdbOta(firmwarePackagePath_);
    } else {
        firmwareUpdater_->startRockchipLoaderUpdate(firmwarePackagePath_);
    }
}

void SettingsPage::onFirmwareStatusChanged(const QString &message) {
    firmwareStatusLabel_->setText(message);
    emit statusMessage(message);
}

void SettingsPage::onFirmwareProgressChanged(int value) {
    firmwareProgress_->setValue(value);
}

void SettingsPage::onFirmwareFinished(bool success, const QString &message) {
    setFirmwareBusy(false);
    if (success) {
        QMessageBox::information(this, tr("Firmware"), message);
    } else {
        QMessageBox::critical(this, tr("Firmware"), message);
    }
}

void SettingsPage::setFirmwareBusy(bool busy) {
    if (busy) {
        selectFirmwareBtn_->setEnabled(false);
        validateFirmwareBtn_->setEnabled(false);
        flashFirmwareBtn_->setEnabled(false);
        return;
    }

    updateFirmwareControls();
}

void SettingsPage::updateFirmwareControls() {
    const bool running = firmwareUpdater_->isRunning();
    const bool hasPackage = !firmwarePackagePath_.isEmpty();
    const auto dependencies = firmwareUpdater_->dependencyStatus();
    const bool validationReady = dependencies.canValidate();
    const bool flashReady = firmwarePackageNeedsRockchipFlasher_
                                ? dependencies.canFlashRockchip()
                                : dependencies.canFlashLegacy();

    selectFirmwareBtn_->setEnabled(!running);
    validateFirmwareBtn_->setEnabled(!running && hasPackage && validationReady);
    flashFirmwareBtn_->setEnabled(!running && hasPackage && firmwarePackageValidated_ &&
                                  firmwarePackageFlashSupported_ && flashReady);

    if (!running && !validationReady) {
        firmwareStatusLabel_->setText(firmwareDependencyMessage(false));
    }
}

QString SettingsPage::firmwareDependencyMessage(bool includeFlasher) const {
    const auto dependencies = firmwareUpdater_->dependencyStatus();
    if (includeFlasher ? dependencies.canFlashRockchip() : dependencies.canValidate()) {
        return {};
    }

    return tr("Missing firmware dependencies: %1")
        .arg(dependencies.missingNames(includeFlasher).join(", "));
}

void SettingsPage::onResetSettings() {
    portCombo_->setCurrentIndex(0);
    keepaliveSpin_->setValue(10);
    cbMinimizeToTray_->setChecked(true);
    cbStartMinimized_->setChecked(false);
    cbAutostart_->setChecked(false);
    emit statusMessage(tr("Settings reset"));
}

void SettingsPage::onSaveSettings() {
    panorama::Config config = panorama::ConfigManager::load_config().value_or(panorama::Config{});
    config.port = selectedPort().toStdString();
    config.keepalive_interval = keepaliveSpin_->value();

    panorama::ConfigManager::save_config(config);

    emit settingsChanged();
    startAutostartCommand(cbAutostart_->isChecked());
}

void SettingsPage::queryAutostartState() {
    if (autostartProcess_->state() != QProcess::NotRunning) {
        return;
    }
    autostartOperation_ = AutostartOperation::Query;
    cbAutostart_->setEnabled(false);
    autostartProcess_->start(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"), QStringLiteral("is-enabled"),
         QStringLiteral("tryx-panorama.service")});
}

void SettingsPage::startAutostartCommand(bool enable) {
    if (autostartProcess_->state() != QProcess::NotRunning ||
        autostartOperation_ != AutostartOperation::None) {
        emit statusMessage(
            tr("The systemd autostart state is still being checked"));
        return;
    }

    autostartOperation_ = enable
        ? AutostartOperation::Enable
        : AutostartOperation::Disable;
    cbAutostart_->setEnabled(false);
    saveBtn_->setEnabled(false);
    autostartProcess_->start(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"),
         enable ? QStringLiteral("enable") : QStringLiteral("disable"),
         QStringLiteral("tryx-panorama.service")});
}

void SettingsPage::onAutostartCommandFinished(
    int exitCode, QProcess::ExitStatus exitStatus) {
    const AutostartOperation completedOperation = autostartOperation_;
    if (completedOperation == AutostartOperation::None) {
        return;
    }
    autostartOperation_ = AutostartOperation::None;
    const QString output =
        QString::fromLocal8Bit(autostartProcess_->readAll()).trimmed();
    const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;

    if (completedOperation == AutostartOperation::Query) {
        autostartEnabled_ = success &&
            (output == QStringLiteral("enabled") ||
             output == QStringLiteral("enabled-runtime") ||
             output == QStringLiteral("linked") ||
             output == QStringLiteral("linked-runtime"));
        cbAutostart_->setChecked(autostartEnabled_);
        cbAutostart_->setEnabled(true);
        if (!success && !output.isEmpty() &&
            output != QStringLiteral("disabled")) {
            cbAutostart_->setToolTip(output);
        }
        return;
    }

    cbAutostart_->setEnabled(true);
    saveBtn_->setEnabled(true);
    if (success) {
        autostartEnabled_ =
            completedOperation == AutostartOperation::Enable;
        cbAutostart_->setChecked(autostartEnabled_);
        emit statusMessage(
            autostartEnabled_
                ? tr("Settings saved; background runtime autostart enabled")
                : tr("Settings saved; background runtime autostart disabled"));
        return;
    }

    cbAutostart_->setChecked(autostartEnabled_);
    const QString error = output.isEmpty()
        ? tr("systemctl failed with exit code %1").arg(exitCode)
        : output;
    emit statusMessage(tr("Failed to change background runtime autostart: %1")
                           .arg(error));
    QMessageBox::critical(
        this, tr("Autostart"),
        tr("Failed to change background runtime autostart: %1")
            .arg(error));
}
