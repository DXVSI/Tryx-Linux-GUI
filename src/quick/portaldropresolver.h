#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantList>

// A drag from a host application into the Flatpak carries host paths the
// sandbox cannot open. Portal-aware sources (Dolphin, GTK file managers) also
// offer an "application/vnd.portal.filetransfer" key; the document portal's
// FileTransfer.RetrieveFiles turns that key into paths exported to this app.
// One instance serves the media drop zone and handles one request at a time.
class PortalDropResolver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    static constexpr const char *kTransferMimeType = "application/vnd.portal.filetransfer";

    explicit PortalDropResolver(QObject *parent = nullptr, int requestTimeoutMs = 15000);
    bool busy() const { return busy_; }
    // Starts a RetrieveFiles request for a drop's transfer key. Returns false
    // when the key is unusable or a request is already running; the caller
    // then falls back to the dropped URLs.
    Q_INVOKABLE bool resolve(const QString &transferKey);
    Q_INVOKABLE void cancel();

    // Exposed for tests: trims the terminator some sources append and rejects
    // keys that cannot be a portal transfer key.
    static QString normalizedTransferKey(const QString &transferKey);
    // Exposed for tests: accepts only absolute local paths and maps each to a
    // file URL inside the resolved document-portal root.
    static QVariantList exportedFileUrls(const QStringList &paths);

signals:
    void busyChanged();
    void resolved(const QVariantList &urls);
    void failed(const QString &message);

private:
    void finish();
    QDBusConnection bus_;
    int requestTimeoutMs_;
    quint64 generation_ = 0;
    bool busy_ = false;
};
