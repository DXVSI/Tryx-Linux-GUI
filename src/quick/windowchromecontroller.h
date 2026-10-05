#pragma once

#include <QObject>
#include <QPointer>

class QWindow;

class WindowChromeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY windowStateChanged)
    Q_PROPERTY(bool maximized READ maximized NOTIFY windowStateChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable
                   WRITE setTrayAvailable
                   NOTIFY trayAvailabilityChanged)
    Q_PROPERTY(bool hideToTrayOnClose READ hideToTrayOnClose
                   WRITE setHideToTrayOnClose
                   NOTIFY closeBehaviorChanged)
    Q_PROPERTY(bool hiddenToTray READ hiddenToTray
                   NOTIFY hiddenToTrayChanged)

public:
    explicit WindowChromeController(QObject *parent = nullptr);

    bool ready() const;
    bool maximized() const;
    bool trayAvailable() const;
    bool hideToTrayOnClose() const;
    bool hiddenToTray() const;
    void setWindow(QWindow *window);
    void setTrayAvailable(bool available);
    void setHideToTrayOnClose(bool enabled);

    Q_INVOKABLE bool startMove();
    Q_INVOKABLE bool startResize(int edges);
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void toggleMaximized();
    Q_INVOKABLE void closeWindow();
    Q_INVOKABLE bool closeWouldHideToTray() const;
    Q_INVOKABLE bool handleCloseRequest();
    Q_INVOKABLE bool hideWindowToTray();
    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void requestExplicitQuit();
    Q_INVOKABLE void approveExplicitQuit();

signals:
    void windowStateChanged();
    void trayAvailabilityChanged();
    void closeBehaviorChanged();
    void hiddenToTrayChanged();
    void explicitQuitRequested();
    void explicitQuitApproved();

private:
    static bool validResizeEdges(Qt::Edges edges);
    void setHiddenToTray(bool hidden);

    QPointer<QWindow> window_;
    bool trayAvailable_ = false;
    bool hideToTrayOnClose_ = true;
    bool hiddenToTray_ = false;
    bool restoreMaximized_ = false;
};
