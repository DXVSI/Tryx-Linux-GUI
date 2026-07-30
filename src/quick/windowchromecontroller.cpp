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

void WindowChromeController::setWindow(QWindow *window) {
    if (window_ == window) {
        return;
    }
    if (window_) {
        disconnect(window_, nullptr, this, nullptr);
    }
    window_ = window;
    if (window_) {
        connect(window_, &QWindow::visibilityChanged,
                this, &WindowChromeController::windowStateChanged);
        connect(window_, &QObject::destroyed, this, [this]() {
            window_.clear();
            emit windowStateChanged();
        });
    }
    emit windowStateChanged();
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
