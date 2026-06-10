#include "mainwindow.h"
#include "devicemanager.h"
#include "homepage.h"
#include "panoramapage.h"
#include "settingspage.h"
#include "traymanager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QStatusBar>
#include <QApplication>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {

    deviceMgr_ = new DeviceManager(this);
    trayMgr_ = new TrayManager(this);

    setupUi();
    setupConnections();

    setWindowTitle(tr("TRYX Panorama Manager"));
    setMinimumSize(640, 480);
    resize(1100, 750);

    trayMgr_->show();

    // Auto-connect on startup
    deviceMgr_->connectDevice(settingsPage_->selectedPort());
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
    auto *centralWidget = new QWidget;
    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Navigation
    navList_ = new QListWidget;
    navList_->setFixedWidth(160);
    navList_->setSpacing(2);
    navList_->addItem(tr("Homepage"));
    navList_->addItem(tr("Panorama"));
    navList_->addItem(tr("Rota"));
    navList_->addItem(tr("Settings"));
    navList_->setCurrentRow(0);

    navList_->setStyleSheet(
        "QListWidget {"
        "  background: #1a1a2e;"
        "  color: #aaa;"
        "  border: none;"
        "  font-size: 14px;"
        "  padding: 8px;"
        "}"
        "QListWidget::item {"
        "  padding: 12px 16px;"
        "  border-radius: 8px;"
        "  margin: 2px 4px;"
        "}"
        "QListWidget::item:selected {"
        "  background: #2d2d4a;"
        "  color: #fff;"
        "}"
        "QListWidget::item:hover {"
        "  background: #252540;"
        "}");

    mainLayout->addWidget(navList_);

    // Pages
    stack_ = new QStackedWidget;
    homepage_ = new Homepage;
    panoramaPage_ = new PanoramaPage(deviceMgr_);
    settingsPage_ = new SettingsPage(deviceMgr_);

    // Rota placeholder
    auto *rotaPage = new QWidget;
    auto *rotaLayout = new QVBoxLayout(rotaPage);
    rotaLayout->setContentsMargins(40, 40, 40, 40);
    rotaLayout->setAlignment(Qt::AlignTop);

    auto *rotaTitle = new QLabel(tr("ROTA"));
    rotaTitle->setStyleSheet("color: #fff; font-size: 22px; font-weight: bold;");
    rotaLayout->addWidget(rotaTitle);

    auto *rotaSubtitle = new QLabel(tr("Lighting & Fan Speed Control"));
    rotaSubtitle->setStyleSheet("color: #aaa; font-size: 13px;");
    rotaLayout->addWidget(rotaSubtitle);

    rotaLayout->addSpacing(30);

    auto *rotaStatus = new QLabel(tr("In Development"));
    rotaStatus->setStyleSheet(
        "color: #DEF750; font-size: 16px; font-weight: bold; "
        "background: #2a2a3e; padding: 16px 32px; border-radius: 8px; border: 1px solid #DEF750;");
    rotaStatus->setAlignment(Qt::AlignCenter);
    rotaLayout->addWidget(rotaStatus, 0, Qt::AlignCenter);

    rotaLayout->addSpacing(20);

    auto *rotaDesc = new QLabel(
        tr("ROTA is the ARGB lighting and fan speed controller for TRYX coolers.\n\n"
           "Planned features:\n"
           "  - ARGB lighting effects (15+ presets)\n"
           "  - Fan speed control (Smart/Fixed modes)\n"
           "  - Per-fan speed curves\n"
           "  - Motherboard ARGB sync"));
    rotaDesc->setStyleSheet("color: #888; font-size: 12px;");
    rotaDesc->setWordWrap(true);
    rotaLayout->addWidget(rotaDesc);

    rotaLayout->addStretch();

    stack_->addWidget(homepage_);
    stack_->addWidget(panoramaPage_);
    stack_->addWidget(rotaPage);
    stack_->addWidget(settingsPage_);

    mainLayout->addWidget(stack_, 1);

    setCentralWidget(centralWidget);

    connect(navList_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

    // Status bar
    statusLabel_ = new QLabel(tr("Disconnected"));
    statusBar()->addPermanentWidget(statusLabel_);
}

void MainWindow::setupConnections() {
    // Device connection
    connect(deviceMgr_, &DeviceManager::deviceConnected, this,
            [this](const QString &pid, const QString &serial,
                   const QString &fw, const QString &) {
                statusLabel_->setText(tr("Connected: %1 (S/N: %2, FW: %3)")
                                          .arg(pid, serial, fw));
                trayMgr_->setConnected(true);
                trayMgr_->showNotification("TRYX Panorama", tr("Device connected"));

                deviceMgr_->startKeepalive(settingsPage_->keepaliveInterval());
                deviceMgr_->refreshMediaList();
            });

    connect(deviceMgr_, &DeviceManager::deviceDisconnected, this, [this]() {
        statusLabel_->setText(tr("Disconnected"));
        trayMgr_->setConnected(false);
    });

    connect(deviceMgr_, &DeviceManager::deviceError, this, [this](const QString &msg) {
        statusLabel_->setText(tr("Error: %1").arg(msg));
    });

    connect(deviceMgr_, &DeviceManager::brightnessChanged, trayMgr_, &TrayManager::setBrightnessValue);

    // Panorama page status
    connect(panoramaPage_, &PanoramaPage::statusMessage, statusBar(),
            [this](const QString &msg) { statusBar()->showMessage(msg, 5000); });
    connect(panoramaPage_, &PanoramaPage::metricsRunningChanged, trayMgr_, &TrayManager::setMetricsRunning);

    // Settings page status
    connect(settingsPage_, &SettingsPage::statusMessage, statusBar(),
            [this](const QString &msg) { statusBar()->showMessage(msg, 5000); });

    // Tray actions
    connect(trayMgr_, &TrayManager::showWindowRequested, this, [this]() {
        show();
        raise();
        activateWindow();
    });
    connect(trayMgr_, &TrayManager::hideWindowRequested, this, &QMainWindow::hide);
    connect(trayMgr_, &TrayManager::quitRequested, this, [this]() {
        minimizeToTray_ = false;
        close();
        QApplication::quit();
    });
    connect(trayMgr_, &TrayManager::brightnessChangeRequested,
            deviceMgr_, &DeviceManager::setBrightness);
    connect(trayMgr_, &TrayManager::metricsToggleRequested, this, [this]() {
        if (panoramaPage_->isMetricsRunning()) {
            panoramaPage_->stopMetrics();
        } else {
            panoramaPage_->startMetrics();
        }
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (minimizeToTray_ && settingsPage_->minimizeToTray()) {
        hide();
        event->ignore();
    } else {
        panoramaPage_->stopMetrics();
        deviceMgr_->disconnectDevice();
        event->accept();
    }
}
