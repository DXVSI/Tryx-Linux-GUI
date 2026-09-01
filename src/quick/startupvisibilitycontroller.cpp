#include "startupvisibilitycontroller.h"

#include "windowchromecontroller.h"

#include <QtGlobal>

StartupVisibilityController::StartupVisibilityController(
    WindowChromeController *windowChrome,
    int autostartResolutionTimeoutMs,
    QObject *parent)
    : QObject(parent), windowChrome_(windowChrome) {
    Q_ASSERT(windowChrome_ != nullptr);
    Q_ASSERT(autostartResolutionTimeoutMs >= 0);
    deadline_.setSingleShot(true);
    deadline_.setInterval(autostartResolutionTimeoutMs);
    connect(
        &deadline_, &QTimer::timeout,
        this, &StartupVisibilityController::showWindow);
}

bool StartupVisibilityController::autostartPending() const {
    return autostartPending_;
}

void StartupVisibilityController::setTrayAvailable(
    bool available) {
    windowChrome_->setTrayAvailable(available);
    if (available) {
        resolveToTrayIfPossible();
    }
}

void StartupVisibilityController::beginAutostart() {
    deadline_.stop();
    autostartPending_ = true;
    resolveToTrayIfPossible();
    if (autostartPending_) {
        deadline_.start();
    }
}

void StartupVisibilityController::showWindow() {
    autostartPending_ = false;
    deadline_.stop();
    windowChrome_->showWindow();
}

void StartupVisibilityController::resolveToTrayIfPossible() {
    if (!autostartPending_) {
        return;
    }
    if (!windowChrome_->hideToTrayOnClose()) {
        showWindow();
        return;
    }
    if (!windowChrome_->trayAvailable()) {
        return;
    }

    autostartPending_ = false;
    deadline_.stop();
    if (!windowChrome_->hideWindowToTray()) {
        windowChrome_->showWindow();
    }
}
