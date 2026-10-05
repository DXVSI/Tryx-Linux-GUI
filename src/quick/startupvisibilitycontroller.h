#pragma once

#include <QObject>
#include <QTimer>

class WindowChromeController;

class StartupVisibilityController final : public QObject {
public:
    explicit StartupVisibilityController(
        WindowChromeController *windowChrome,
        int autostartResolutionTimeoutMs = 2500,
        QObject *parent = nullptr);

    bool autostartPending() const;
    void setTrayAvailable(bool available);
    void beginAutostart();
    void showWindow();

private:
    void resolveToTrayIfPossible();

    WindowChromeController *windowChrome_ = nullptr;
    QTimer deadline_;
    bool autostartPending_ = false;
};
