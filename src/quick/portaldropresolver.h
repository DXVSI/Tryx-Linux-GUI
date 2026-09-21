#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantList>

// A drag from a host application into the Flatpak carries host paths the
// sandbox cannot open. Portal-aware sources (Dolphin, GTK file managers) also
// offer an "application/vnd.portal.filetransfer" key; the document portal's
// FileTransfer.RetrieveFiles turns that key into paths exported to this app.
//
// The key only lives as long as the drag: KDE sources stop the transfer as
// soon as the drop is finished, and a later RetrieveFiles fails with "Invalid
// transfer". The files are therefore retrieved synchronously inside the drop
// handler, before the drop is accepted, with a bounded wait.
class PortalDropResolver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    static constexpr const char *kTransferMimeType = "application/vnd.portal.filetransfer";

    explicit PortalDropResolver(QObject *parent = nullptr, int requestTimeoutMs = 5000);
    bool busy() const { return busy_; }
    // Retrieves the files of a drop's transfer key and emits resolved() or
    // failed() before returning. Must be called from the drop handler, before
    // the drop is accepted. Returns false when the key is unusable or a
    // request is already running; the caller then falls back to the dropped
    // URLs.
    Q_INVOKABLE bool resolve(const QString &transferKey);

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
    void setBusy(bool busy);
    QDBusConnection bus_;
    int requestTimeoutMs_;
    bool busy_ = false;
};
