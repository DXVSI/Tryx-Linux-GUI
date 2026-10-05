#include "portalfilechooser.h"
#include "../packagingcontext.h"

#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QUuid>

namespace {
const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
constexpr int callTimeoutMs = 2000;
}

PortalFileChooser::PortalFileChooser(Mode mode, QObject *parent, int timeout)
    : QObject(parent), mode_(mode), bus_(QDBusConnection::sessionBus()) {
    deadline_.setSingleShot(true);
    deadline_.setInterval(qMax(1, timeout));
    connect(&deadline_, &QTimer::timeout, this, [this]() {
        fail(tr("The desktop file chooser timed out. Please try again."));
    });
}
PortalFileChooser::~PortalFileChooser() { reset(); }

void PortalFileChooser::open(const QString &title, const QUrl &folder) {
    if (busy_)
        return;
    if (!bus_.isConnected()) {
        emit failed(tr("The desktop file portal is unavailable: no session bus."));
        return;
    }
    busy_ = true;
    const quint64 generation = ++generation_;
    emit busyChanged();
    deadline_.start();
    // Properties.Get may activate the portal. Subsequent calls are pinned to
    // its unique owner; no result from a replacement portal is accepted.
    auto message = QDBusMessage::createMethodCall(portalService, portalPath,
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    message.setArguments({QStringLiteral("org.freedesktop.portal.FileChooser"),
                          QStringLiteral("version")});
    auto *pending = new QDBusPendingCallWatcher(bus_.asyncCall(message, callTimeoutMs), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this,
            [this, pending, generation, title, folder]() {
        const QDBusPendingReply<QDBusVariant> reply = *pending;
        pending->deleteLater();
        if (generation != generation_ || !busy_)
            return;
        const QVariant version = reply.isError() ? QVariant() : reply.value().variant();
        const uint minimum = mode_ == Mode::Directory ? 3 : 1;
        if (reply.isError() || version.metaType().id() != QMetaType::UInt ||
            version.toUInt() < minimum) {
            fail(tr("A compatible desktop FileChooser portal is unavailable: %1")
                 .arg(reply.isError() ? reply.error().message() : tr("unsupported portal version")));
            return;
        }
        auto ownerQuery = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("GetNameOwner"));
        ownerQuery.setArguments({portalService});
        auto *ownerCall = new QDBusPendingCallWatcher(bus_.asyncCall(ownerQuery, callTimeoutMs), this);
        connect(ownerCall, &QDBusPendingCallWatcher::finished, this,
                [this, ownerCall, generation, title, folder]() {
            const QDBusPendingReply<QString> ownerReply = *ownerCall;
            ownerCall->deleteLater();
            if (generation != generation_ || !busy_)
                return;
            if (ownerReply.isError() || !ownerReply.value().startsWith(':')) {
                fail(tr("The desktop file portal exited during startup."));
                return;
            }
            owner_ = ownerReply.value();
            ownerWatcher_ = new QDBusServiceWatcher(portalService, bus_,
                QDBusServiceWatcher::WatchForOwnerChange, this);
            connect(ownerWatcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
                    [this, generation](const QString &, const QString &, const QString &next) {
                if (generation == generation_ && busy_ && next != owner_)
                    fail(tr("The desktop file portal restarted. Please select the file again."));
            });
            // Recheck the version on the pinned owner. A replacement between
            // activation and GetNameOwner must not inherit the old version.
            auto versionQuery = QDBusMessage::createMethodCall(owner_, portalPath,
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
            versionQuery.setAutoStartService(false);
            versionQuery.setArguments({QStringLiteral("org.freedesktop.portal.FileChooser"),
                                       QStringLiteral("version")});
            auto *versionCall = new QDBusPendingCallWatcher(
                bus_.asyncCall(versionQuery, callTimeoutMs), this);
            connect(versionCall, &QDBusPendingCallWatcher::finished, this,
                    [this, versionCall, generation, title, folder]() {
                const QDBusPendingReply<QDBusVariant> pinnedReply = *versionCall;
                versionCall->deleteLater();
                if (generation != generation_ || !busy_)
                    return;
                const QVariant pinnedVersion = pinnedReply.isError()
                    ? QVariant() : pinnedReply.value().variant();
                if (pinnedReply.isError() || pinnedVersion.metaType().id() != QMetaType::UInt ||
                    pinnedVersion.toUInt() < (mode_ == Mode::Directory ? 3U : 1U)) {
                    fail(tr("A compatible desktop FileChooser portal is unavailable."));
                    return;
                }
                openRequest(title, folder, generation);
            });
        });
    });
}

void PortalFileChooser::openRequest(const QString &title, const QUrl &folder,
                                    quint64 generation) {
    token_ = QStringLiteral("tryx_") + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = bus_.baseService().mid(1);
    sender.replace('.', '_');
    requestPath_ = portalPath + QStringLiteral("/request/") + sender + '/' + token_;
    if (!bus_.connect(owner_, requestPath_, requestInterface, QStringLiteral("Response"),
                      this, SLOT(response(uint,QVariantMap,QDBusMessage)))) {
        fail(tr("Could not subscribe to the desktop file chooser response."));
        return;
    }
    QVariantMap options{{QStringLiteral("handle_token"), token_},
                        {QStringLiteral("modal"), true},
                        {QStringLiteral("multiple"), false},
                        {QStringLiteral("directory"), mode_ == Mode::Directory}};
    if (folder.isLocalFile() && folder.host().isEmpty() &&
        QDir::isAbsolutePath(folder.toLocalFile()) && !folder.toLocalFile().contains(QChar::Null)) {
        QByteArray encoded = QFile::encodeName(folder.toLocalFile());
        encoded.append('\0');
        options.insert(QStringLiteral("current_folder"), encoded);
    }
    auto message = QDBusMessage::createMethodCall(owner_, portalPath,
        QStringLiteral("org.freedesktop.portal.FileChooser"), QStringLiteral("OpenFile"));
    message.setAutoStartService(false);
    // An empty parent_window is valid on both Wayland and X11, without relying
    // on private Qt platform APIs. The portal still supplies its system dialog.
    message.setArguments({QString(), title, options});
    auto *pending = new QDBusPendingCallWatcher(bus_.asyncCall(message, callTimeoutMs), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending, generation]() {
        const QDBusPendingReply<QDBusObjectPath> reply = *pending;
        pending->deleteLater();
        if (generation != generation_ || !busy_)
            return;
        if (reply.isError() || reply.value().path() != requestPath_) {
            fail(tr("The desktop file chooser could not be opened: %1")
                 .arg(reply.isError() ? reply.error().message() : tr("unexpected request identity")));
            return;
        }
        acknowledged_ = true;
        if (earlyResponse_) {
            const auto result = *earlyResponse_;
            earlyResponse_.reset();
            completeResponse(result.first, result.second);
        }
    });
}

void PortalFileChooser::response(uint result, const QVariantMap &results,
                                 const QDBusMessage &message) {
    if (!busy_ || responseReceived_ || message.service() != owner_ || message.path() != requestPath_)
        return;
    responseReceived_ = true;
    if (!acknowledged_) {
        if (!earlyResponse_)
            earlyResponse_ = qMakePair(result, results);
        return;
    }
    completeResponse(result, results);
}

void PortalFileChooser::completeResponse(uint result, const QVariantMap &results) {
    if (result == 1) {
        cancel();
        return;
    }
    if (result != 0) {
        fail(tr("The desktop file chooser could not complete the request."));
        return;
    }
    const QStringList uris = qdbus_cast<QStringList>(results.value(QStringLiteral("uris")));
    const QUrl url(uris.size() == 1 ? uris.first() : QString(), QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile() || !url.host().isEmpty() ||
        url.hasQuery() || url.hasFragment() || !QDir::isAbsolutePath(url.toLocalFile()) ||
        url.toLocalFile().contains(QChar::Null)) {
        fail(tr("The desktop file chooser did not return exactly one local file or folder."));
        return;
    }
    // Fence the result even if NameOwnerChanged is still queued locally.
    const quint64 generation = generation_;
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("GetNameOwner"));
    message.setArguments({portalService});
    auto *pending = new QDBusPendingCallWatcher(bus_.asyncCall(message, callTimeoutMs), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this, pending, generation, url]() {
        const QDBusPendingReply<QString> reply = *pending;
        pending->deleteLater();
        if (generation != generation_ || !busy_)
            return;
        if (reply.isError() || reply.value() != owner_) {
            fail(tr("The desktop file portal changed before the selection was accepted."));
            return;
        }
        reset();
        emit selected(tryx::packaging::resolveDocumentsPortalAlias(url));
    });
}

void PortalFileChooser::cancel() {
    if (!busy_)
        return;
    reset();
    emit cancelled();
}

void PortalFileChooser::fail(const QString &message) {
    reset();
    emit failed(message);
}

void PortalFileChooser::reset() {
    ++generation_;
    deadline_.stop();
    if (!owner_.isEmpty() && !requestPath_.isEmpty()) {
        bus_.disconnect(owner_, requestPath_, requestInterface, QStringLiteral("Response"),
                        this, SLOT(response(uint,QVariantMap,QDBusMessage)));
        auto close = QDBusMessage::createMethodCall(owner_, requestPath_, requestInterface,
                                                   QStringLiteral("Close"));
        close.setAutoStartService(false);
        bus_.call(close, QDBus::NoBlock);
    }
    if (ownerWatcher_) {
        ownerWatcher_->disconnect(this);
        ownerWatcher_->deleteLater();
    }
    ownerWatcher_ = nullptr;
    owner_.clear();
    requestPath_.clear();
    token_.clear();
    earlyResponse_.reset();
    acknowledged_ = false;
    responseReceived_ = false;
    if (busy_) {
        busy_ = false;
        emit busyChanged();
    }
}
