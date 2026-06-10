#include "settingspage.h"
#include "devicemanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QMessageBox>
#include <QDir>
#include <QProcess>

#include <panorama/config.hpp>

SettingsPage::SettingsPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr) {
    setupUi();
    loadSettings();
}

void SettingsPage::setupUi() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Port settings
    auto *portGroup = new QGroupBox(tr("Connection"));
    auto *portLayout = new QGridLayout(portGroup);

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

    mainLayout->addWidget(portGroup);

    connect(refreshPortsBtn_, &QPushButton::clicked, this, &SettingsPage::onRefreshPorts);

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

    // Language
    auto *languageGroup = new QGroupBox(tr("Language"));
    auto *languageLayout = new QGridLayout(languageGroup);

    languageCombo_ = new QComboBox;
    languageCombo_->addItem(tr("System language"), "system");
    languageCombo_->addItem(tr("English"), "en");
    languageCombo_->addItem(tr("Russian"), "ru");

    languageLayout->addWidget(new QLabel(tr("Interface language:")), 0, 0);
    languageLayout->addWidget(languageCombo_, 0, 1);

    mainLayout->addWidget(languageGroup);

    // Device info
    auto *infoGroup = new QGroupBox(tr("Device"));
    auto *infoLayout = new QHBoxLayout(infoGroup);

    deviceInfoBtn_ = new QPushButton(tr("Device information"));
    infoLayout->addWidget(deviceInfoBtn_);
    infoLayout->addStretch();

    mainLayout->addWidget(infoGroup);

    connect(deviceInfoBtn_, &QPushButton::clicked, this, &SettingsPage::onShowDeviceInfo);

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
        loadedLanguage_ = QString::fromStdString(config->language);
        int langIndex = languageCombo_->findData(loadedLanguage_);
        if (langIndex < 0) {
            loadedLanguage_ = "system";
            langIndex = languageCombo_->findData(loadedLanguage_);
        }
        languageCombo_->setCurrentIndex(langIndex);
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

QString SettingsPage::selectedLanguage() const {
    return languageCombo_->currentData().toString();
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
    if (!deviceMgr_->isConnected()) {
        QMessageBox::information(this, tr("Device"), tr("Device not connected"));
        return;
    }

    // Trigger handshake - info will come through signals
    emit statusMessage(tr("Requesting device information..."));
}

void SettingsPage::onResetSettings() {
    portCombo_->setCurrentIndex(0);
    keepaliveSpin_->setValue(10);
    languageCombo_->setCurrentIndex(languageCombo_->findData("system"));
    cbMinimizeToTray_->setChecked(true);
    cbStartMinimized_->setChecked(false);
    cbAutostart_->setChecked(false);
    emit statusMessage(tr("Settings reset"));
}

void SettingsPage::onSaveSettings() {
    panorama::Config config;
    config.port = selectedPort().toStdString();
    config.keepalive_interval = keepaliveSpin_->value();
    config.brightness = 75;
    config.language = selectedLanguage().toStdString();

    panorama::ConfigManager::save_config(config);

    // Handle autostart
    if (cbAutostart_->isChecked()) {
        QString serviceDir = QDir::homePath() + "/.config/systemd/user";
        QDir().mkpath(serviceDir);
        // The service file is managed by the user through the CLI
    }

    emit settingsChanged();
    if (selectedLanguage() != loadedLanguage_) {
        loadedLanguage_ = selectedLanguage();
        emit statusMessage(tr("Settings saved. Restart the application to apply language changes."));
    } else {
        emit statusMessage(tr("Settings saved"));
    }
}
