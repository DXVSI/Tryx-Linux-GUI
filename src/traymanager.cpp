#include "traymanager.h"
#include <QApplication>
#include <QIcon>
#include <QStyle>

TrayManager::TrayManager(QObject *parent)
    : QObject(parent) {
    setupTray();
}

void TrayManager::setupTray() {
    trayIcon_ = new QSystemTrayIcon(this);
    const QIcon appIcon(":/tryx-panorama.png");
    trayIcon_->setIcon(appIcon.isNull()
                           ? QApplication::style()->standardIcon(QStyle::SP_ComputerIcon)
                           : appIcon);

    trayMenu_ = new QMenu;

    showHideAction_ = trayMenu_->addAction(tr("Hide"));
    connect(showHideAction_, &QAction::triggered, this, [this]() {
        if (windowVisible_) {
            windowVisible_ = false;
            showHideAction_->setText(tr("Show"));
            emit hideWindowRequested();
        } else {
            windowVisible_ = true;
            showHideAction_->setText(tr("Hide"));
            emit showWindowRequested();
        }
    });

    trayMenu_->addSeparator();

    metricsAction_ = trayMenu_->addAction(tr("Start Metrics"));
    connect(metricsAction_, &QAction::triggered, this, &TrayManager::metricsToggleRequested);

    trayMenu_->addSeparator();

    // Brightness submenu
    brightnessMenu_ = trayMenu_->addMenu(tr("Brightness"));
    for (int val : {25, 50, 75, 100}) {
        auto *action = brightnessMenu_->addAction(QString("%1%").arg(val));
        connect(action, &QAction::triggered, this, [this, val]() {
            emit brightnessChangeRequested(val);
        });
    }

    trayMenu_->addSeparator();

    auto *quitAction = trayMenu_->addAction(tr("Quit"));
    connect(quitAction, &QAction::triggered, this, &TrayManager::quitRequested);

    trayIcon_->setContextMenu(trayMenu_);

    connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    if (windowVisible_) {
                        windowVisible_ = false;
                        showHideAction_->setText(tr("Show"));
                        emit hideWindowRequested();
                    } else {
                        windowVisible_ = true;
                        showHideAction_->setText(tr("Hide"));
                        emit showWindowRequested();
                    }
                }
            });

    updateTooltip();
}

void TrayManager::show() {
    trayIcon_->show();
}

void TrayManager::hide() {
    trayIcon_->hide();
}

void TrayManager::showNotification(const QString &title, const QString &message) {
    trayIcon_->showMessage(title, message, QSystemTrayIcon::Information, 3000);
}

void TrayManager::setConnected(bool connected) {
    connected_ = connected;
    updateTooltip();
}

void TrayManager::setMetricsRunning(bool running) {
    metricsRunning_ = running;
    metricsAction_->setText(running ? tr("Stop Metrics") : tr("Start Metrics"));
    updateTooltip();
}

void TrayManager::setBrightnessValue(int value) {
    brightness_ = value;
    updateTooltip();
}

void TrayManager::updateTooltip() {
    QString tooltip = "TRYX Panorama Manager";
    if (connected_) {
        tooltip += "\n" + tr("Connected");
        tooltip += "\n" + tr("Brightness: %1%").arg(brightness_);
        if (metricsRunning_) {
            tooltip += "\n" + tr("Metrics active");
        }
    } else {
        tooltip += "\n" + tr("Disconnected");
    }
    trayIcon_->setToolTip(tooltip);
}
