#include "windowchromecontroller.h"

#include <QWindow>

WindowChromeController::WindowChromeController(QObject *parent)
    : QObject(parent) {}

bool WindowChromeController::ready() const {
    return !window_.isNull();
}

bool WindowChromeController::maximized() const {
    return window_ &&
           window_->visibility() == QWindow::Maximized;
}

bool WindowChromeController::trayAvailable() const {
    return trayAvailable_;
}

bool WindowChromeController::hiddenToTray() const {
    return hiddenToTray_;
}

void WindowChromeController::setWindow(QWindow *window) {
    if (window_ == window) {
        return;
    }
    if (window_) {
        disconnect(window_, nullptr, this, nullptr);
    }
    setHiddenToTray(false);
    restoreMaximized_ = false;
    window_ = window;
    if (window_) {
        connect(window_, &QWindow::visibilityChanged,
                this, &WindowChromeController::windowStateChanged);
        connect(window_, &QObject::destroyed, this, [this]() {
            window_.clear();
            setHiddenToTray(false);
            emit windowStateChanged();
        });
    }
    emit windowStateChanged();
}

void WindowChromeController::setTrayAvailable(bool available) {
    if (trayAvailable_ == available) {
        return;
    }
    trayAvailable_ = available;
    emit trayAvailabilityChanged();

    // Never leave the GUI alive but unreachable after the desktop removes
    // its StatusNotifier host or the watcher process restarts.
    if (!trayAvailable_ && hiddenToTray_) {
        showWindow();
    }
}

bool WindowChromeController::startMove() {
    if (!window_ || maximized()) {
        return false;
    }
    return window_->startSystemMove();
}

bool WindowChromeController::startResize(int edges) {
    const Qt::Edges requested(edges);
    if (!window_ || maximized() ||
        !validResizeEdges(requested)) {
        return false;
    }
    return window_->startSystemResize(requested);
}

void WindowChromeController::minimize() {
    if (window_) {
        window_->showMinimized();
    }
}

void WindowChromeController::toggleMaximized() {
    if (!window_) {
        return;
    }
    if (maximized()) {
        window_->showNormal();
    } else {
        window_->showMaximized();
    }
}

void WindowChromeController::closeWindow() {
    if (window_) {
        window_->close();
    }
}

bool WindowChromeController::handleCloseRequest() {
    if (!window_ || !trayAvailable_) {
        return false;
    }

    restoreMaximized_ =
        window_->visibility() == QWindow::Maximized;
    setHiddenToTray(true);
    window_->hide();
    return true;
}

void WindowChromeController::showWindow() {
    if (!window_) {
        setHiddenToTray(false);
        return;
    }

    const bool restoreMaximized = restoreMaximized_;
    setHiddenToTray(false);
    if (restoreMaximized) {
        window_->showMaximized();
    } else if (window_->visibility() == QWindow::Minimized) {
        window_->showNormal();
    } else {
        window_->show();
    }
    restoreMaximized_ = false;
    window_->raise();
    window_->requestActivate();
}

bool WindowChromeController::validResizeEdges(
    Qt::Edges edges) {
    return edges == Qt::LeftEdge ||
           edges == Qt::RightEdge ||
           edges == Qt::TopEdge ||
           edges == Qt::BottomEdge ||
           edges == (Qt::LeftEdge | Qt::TopEdge) ||
           edges == (Qt::RightEdge | Qt::TopEdge) ||
           edges == (Qt::LeftEdge | Qt::BottomEdge) ||
           edges == (Qt::RightEdge | Qt::BottomEdge);
}

void WindowChromeController::setHiddenToTray(bool hidden) {
    if (hiddenToTray_ == hidden) {
        return;
    }
    hiddenToTray_ = hidden;
    emit hiddenToTrayChanged();
}
