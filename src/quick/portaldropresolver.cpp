#include "portaldropresolver.h"
#include "../packagingcontext.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QUrl>
#include <QVariantMap>

namespace {
const QString documentsService = QStringLiteral("org.freedesktop.portal.Documents");
const QString documentsPath = QStringLiteral("/org/freedesktop/portal/documents");
const QString fileTransferInterface = QStringLiteral("org.freedesktop.portal.FileTransfer");
constexpr qsizetype kMaximumKeyLength = 256;
}

PortalDropResolver::PortalDropResolver(QObject *parent, int requestTimeoutMs)
    : QObject(parent), bus_(QDBusConnection::sessionBus()),
      requestTimeoutMs_(qMax(1, requestTimeoutMs)) {}

QString PortalDropResolver::normalizedTransferKey(const QString &transferKey) {
    QString key = transferKey;
    while (key.endsWith(QChar::Null)) {
        key.chop(1);
    }
    key = key.trimmed();
    if (key.isEmpty() || key.size() > kMaximumKeyLength) {
        return {};
    }
    for (const QChar character : key) {
        if (character.isNull() || character.isSpace() || character.unicode() > 0x7e ||
            character.unicode() < 0x21) {
            return {};
        }
    }
    return key;
}

QVariantList PortalDropResolver::exportedFileUrls(const QStringList &paths) {
    QVariantList urls;
    for (const QString &path : paths) {
        if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)) {
            return {};
        }
        urls.append(tryx::packaging::resolveDocumentsPortalAlias(QUrl::fromLocalFile(path)));
    }
    return urls;
}

bool PortalDropResolver::resolve(const QString &transferKey) {
    if (busy_) {
        return false;
    }
    const QString key = normalizedTransferKey(transferKey);
    if (key.isEmpty() || !bus_.isConnected()) {
        return false;
    }
    busy_ = true;
    const quint64 generation = ++generation_;
    emit busyChanged();
    auto message = QDBusMessage::createMethodCall(documentsService, documentsPath,
                                                  fileTransferInterface,
                                                  QStringLiteral("RetrieveFiles"));
    message.setArguments({key, QVariantMap{}});
    auto *pending =
        new QDBusPendingCallWatcher(bus_.asyncCall(message, requestTimeoutMs_), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending, generation]() {
        const QDBusPendingReply<QStringList> reply = *pending;
        pending->deleteLater();
        if (generation != generation_ || !busy_) {
            return;
        }
        finish();
        if (reply.isError()) {
            emit failed(tr("The desktop portal could not share the dropped file: %1")
                            .arg(reply.error().message()));
            return;
        }
        const QVariantList urls = exportedFileUrls(reply.value());
        if (urls.isEmpty()) {
            emit failed(tr("The desktop portal did not share a local file for this drop."));
            return;
        }
        emit resolved(urls);
    });
    return true;
}

void PortalDropResolver::cancel() {
    if (!busy_) {
        return;
    }
    ++generation_;
    finish();
}

void PortalDropResolver::finish() {
    if (busy_) {
        busy_ = false;
        emit busyChanged();
    }
}
