#pragma once

#include <QObject>
#include <QPointer>

class QWindow;

class WindowChromeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY windowStateChanged)
    Q_PROPERTY(bool maximized READ maximized NOTIFY windowStateChanged)

public:
    explicit WindowChromeController(QObject *parent = nullptr);

    bool ready() const;
    bool maximized() const;
    void setWindow(QWindow *window);

    Q_INVOKABLE bool startMove();
    Q_INVOKABLE bool startResize(int edges);
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void toggleMaximized();
    Q_INVOKABLE void closeWindow();

signals:
    void windowStateChanged();

private:
    static bool validResizeEdges(Qt::Edges edges);

    QPointer<QWindow> window_;
};
