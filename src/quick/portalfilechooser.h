#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <optional>

class QDBusServiceWatcher;

// File access is granted by the desktop portal, never by widening the sandbox.
// Each instance represents one independent UI workflow and at most one request.
class PortalFileChooser final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    enum class Mode { MediaFile, Directory };
    explicit PortalFileChooser(Mode mode, QObject *parent = nullptr,
                               int requestTimeoutMs = 120000);
    ~PortalFileChooser() override;
    bool busy() const { return busy_; }
    Q_INVOKABLE void open(const QString &title, const QUrl &currentFolder = {});
    Q_INVOKABLE void cancel();
signals:
    void busyChanged();
    void selected(const QUrl &url);
    void cancelled();
    void failed(const QString &message);
private slots:
    void response(uint result, const QVariantMap &results, const QDBusMessage &message);
private:
    void openRequest(const QString &title, const QUrl &folder, quint64 generation);
    void completeResponse(uint result, const QVariantMap &results);
    void fail(const QString &message);
    void reset();
    Mode mode_;
    QDBusConnection bus_;
    QTimer deadline_;
    QDBusServiceWatcher *ownerWatcher_ = nullptr;
    QString owner_;
    QString requestPath_;
    QString token_;
    quint64 generation_ = 0;
    bool busy_ = false;
    bool acknowledged_ = false;
    bool responseReceived_ = false;
    std::optional<QPair<uint, QVariantMap>> earlyResponse_;
};
