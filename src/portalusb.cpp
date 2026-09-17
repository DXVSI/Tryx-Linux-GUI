#include "portalusb.h"
#include "printerproductprofile.h"

#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <initializer_list>
#include <fcntl.h>
#include <sys/stat.h>

namespace tryx::portal_usb {
namespace {
const QString kDesktopPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kUsbInterface = QStringLiteral("org.freedesktop.portal.Usb");
const QString kRequestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString kSessionInterface = QStringLiteral("org.freedesktop.portal.Session");
const QString kBusService = QStringLiteral("org.freedesktop.DBus");
const QString kBusPath = QStringLiteral("/org/freedesktop/DBus");
constexpr qsizetype kMaximumDevices = 64;

QString structuredValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString boolText(bool value) {
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

// Journal-only trace of the portal access lifecycle for remote diagnosis.
// Device ids, serials and object paths are never logged; product ids are
// public USB identities.
void logPortalEvent(const QString &event,
                    std::initializer_list<QPair<QString, QString>> fields = {}) {
    QStringList parts{QStringLiteral("tryx_usb_portal"),
                      QStringLiteral("event=") + structuredValue(event)};
    for (const auto &field : fields)
        parts.append(field.first + QLatin1Char('=') + structuredValue(field.second));
    qInfo().noquote() << parts.join(QLatin1Char(' '));
}

QString token() {
    return QUuid::createUuid().toString(QUuid::Id128);
}

QString objectPath(const QString &kind, const QDBusConnection &bus,
                   const QString &handleToken) {
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return kDesktopPath + QLatin1Char('/') + kind + QLatin1Char('/') +
        sender + QLatin1Char('/') + handleToken;
}

QVariantMap variantMap(const QVariant &value) {
    if (value.metaType().id() == QMetaType::QVariantMap)
        return value.toMap();
    if (value.metaType().id() == qMetaTypeId<QDBusArgument>())
        return qdbus_cast<QVariantMap>(value);
    return {};
}

bool boolean(const QVariantMap &properties, const QString &name) {
    const QVariant value = properties.value(name);
    return value.metaType().id() == QMetaType::Bool && value.toBool();
}

QString textProperty(const QVariantMap &properties, const QString &name) {
    const QVariant value = properties.value(name);
    if (value.metaType().id() != QMetaType::QString)
        return {};
    QString text = value.toString().left(256);
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    return text.trimmed();
}

quint16 hexId(const QVariantMap &properties, const QString &name) {
    const QString value = textProperty(properties, name);
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{4}$"));
    return pattern.match(value).hasMatch() ? value.toUShort(nullptr, 16) : 0;
}

bool isDeviceId(const QString &id) {
    static const QRegularExpression control(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    return !id.isEmpty() && id.size() <= 256 && !id.contains(control);
}

bool usableDescriptor(const QDBusUnixFileDescriptor &descriptor) {
    if (!descriptor.isValid())
        return false;
    struct stat status {};
    const int flags = ::fcntl(descriptor.fileDescriptor(), F_GETFL);
    // The actual USB VID/PID/interface are checked by libusb before claiming.
    return flags >= 0 && (flags & O_ACCMODE) == O_RDWR &&
        ::fstat(descriptor.fileDescriptor(), &status) == 0 &&
        S_ISCHR(status.st_mode);
}
}

QDBusArgument &operator<<(QDBusArgument &a, const Device &d) {
    a.beginStructure(); a << d.id << d.properties; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Device &d) {
    a.beginStructure(); a >> d.id >> d.properties; a.endStructure(); return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const Event &e) {
    a.beginStructure(); a << e.action << e.id << e.properties; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Event &e) {
    a.beginStructure(); a >> e.action >> e.id >> e.properties; a.endStructure(); return a;
}
void registerTypes() {
    qDBusRegisterMetaType<Device>(); qDBusRegisterMetaType<Devices>();
    qDBusRegisterMetaType<Event>(); qDBusRegisterMetaType<Events>();
    qRegisterMetaType<Devices>("Devices");
    qRegisterMetaType<Events>("Events");
}
Registry &Registry::instance() { static Registry registry; return registry; }

Snapshot Registry::snapshot() const {
    QMutexLocker lock(&mutex_);
    Snapshot snapshot{monitoring_, {}, error_, acquisitionPending_};
    for (const Entry &entry : entries_)
        snapshot.devices.append(entry.info);
    std::sort(snapshot.devices.begin(), snapshot.devices.end(),
              [](const DeviceInfo &a, const DeviceInfo &b) { return a.id < b.id; });
    return snapshot;
}

QDBusUnixFileDescriptor Registry::descriptorFor(
    const QString &endpoint, quint16 productId) const {
    QMutexLocker lock(&mutex_);
    if (monitoring_) {
        for (const Entry &entry : entries_) {
            if (entry.info.endpoint == endpoint && entry.info.productId == productId &&
                entry.info.granted)
                return entry.descriptor;
        }
    }
    return {};
}

bool Registry::hasGrant(const QString &endpoint, quint16 productId) const {
    return descriptorFor(endpoint, productId).isValid();
}

Access::Access(Registry &registry, QObject *parent) : Access(registry, Options{}, parent) {}
Access::Access(Registry &registry, const Options &options, QObject *parent)
    : QObject(parent), registry_(registry), options_(options), bus_(QString()) {
    registerTypes();
    permissionDeadline_.setSingleShot(true);
    connect(&permissionDeadline_, &QTimer::timeout, this, [this]() {
        logPortalEvent(QStringLiteral("acquire_timeout"));
        fail(tr("The USB permission request timed out. Restart the Flatpak background runtime to try again."));
    });
    connectionCheck_.setInterval(250);
    connect(&connectionCheck_, &QTimer::timeout, this, [this]() {
        if (running_ && !bus_.isConnected())
            fail(tr("The USB portal connection was lost; device access has stopped."));
    });
}

Access::~Access() { stop(); }

bool Access::start() {
    if (running_)
        return true;
    connectionName_ = QStringLiteral("tryx-usb-portal-") + token();
    bus_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName_);
    running_ = true;
    ++generation_;
    {
        QMutexLocker lock(&registry_.mutex_);
        registry_.entries_.clear();
        registry_.monitoring_ = false;
        registry_.acquisitionPending_ = false;
        registry_.error_.clear();
    }
    logPortalEvent(QStringLiteral("started"));
    if (!bus_.isConnected() ||
        !(bus_.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing)) {
        fail(tr("The USB portal requires a local D-Bus connection with file descriptor passing."));
        return false;
    }
    watcher_ = new QDBusServiceWatcher(
        options_.service, bus_, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &oldOwner, const QString &newOwner) {
        if (running_ && !owner_.isEmpty() && oldOwner == owner_ && newOwner != owner_)
            fail(tr("The USB portal owner changed; device access has stopped."));
    });
    connectionCheck_.start();
    resolveOwner(true);
    return true;
}

void Access::call(const QString &destination, const QString &path,
                  const QString &interface, const QString &method,
                  const QVariantList &arguments, ReplyHandler handler) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        destination, path, interface, method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    const quint64 generation = generation_;
    auto *pending = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, qMax(1, options_.callTimeoutMs)), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this,
            [this, pending, generation, handler = std::move(handler)]() {
        const QDBusMessage reply = pending->reply();
        pending->deleteLater();
        if (running_ && generation == generation_)
            handler(reply);
    });
}

void Access::resolveOwner(bool allowActivation) {
    call(kBusService, kBusPath, kBusService, QStringLiteral("GetNameOwner"),
         {options_.service}, [this, allowActivation](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            if (allowActivation && reply.errorName() ==
                QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner")) {
                call(kBusService, kBusPath, kBusService,
                     QStringLiteral("StartServiceByName"),
                     {options_.service, uint{0}}, [this](const QDBusMessage &activation) {
                    if (activation.type() == QDBusMessage::ErrorMessage) {
                        fail(tr("USB portal is unavailable: %1").arg(activation.errorMessage()));
                    } else {
                        resolveOwner(false);
                    }
                });
                return;
            }
            fail(tr("USB portal is unavailable: %1").arg(reply.errorMessage()));
            return;
        }
        if (reply.arguments().size() != 1 ||
            reply.arguments().first().metaType().id() != QMetaType::QString ||
            !reply.arguments().first().toString().startsWith(QLatin1Char(':'))) {
            fail(tr("The USB portal returned an invalid D-Bus owner."));
            return;
        }
        owner_ = reply.arguments().first().toString();
        createSession();
    });
}

void Access::createSession() {
    const QString handleToken = token();
    sessionPath_ = objectPath(QStringLiteral("session"), bus_, handleToken);
    // Subscribe before CreateSession: initial DeviceEvents may precede its reply.
    if (!bus_.connect(owner_, kDesktopPath, kUsbInterface, QStringLiteral("DeviceEvents"),
                      this, SLOT(deviceEvents(QDBusObjectPath,Events,QDBusMessage))) ||
        !bus_.connect(owner_, sessionPath_, kSessionInterface, QStringLiteral("Closed"),
                      this, SLOT(sessionClosed(QVariantMap,QDBusMessage)))) {
        fail(tr("Cannot subscribe to USB portal device events."));
        return;
    }
    call(owner_, kDesktopPath, kUsbInterface, QStringLiteral("CreateSession"),
         {QVariantMap{{QStringLiteral("session_handle_token"), handleToken}}},
         [this](const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            fail(tr("USB portal monitoring is unavailable: %1").arg(reply.errorMessage()));
            return;
        }
        if (reply.arguments().size() != 1 ||
            qdbus_cast<QDBusObjectPath>(reply.arguments().first()).path() != sessionPath_) {
            fail(tr("The USB portal returned an unexpected session."));
            return;
        }
        verifyOwner([this]() {
            sessionReady_ = true;
            logPortalEvent(QStringLiteral("session_created"));
            {
                QMutexLocker lock(&registry_.mutex_);
                registry_.monitoring_ = true;
            }
            if (!maybeAcquire())
                publish();
        });
    });
}

void Access::verifyOwner(std::function<void()> handler) {
    call(kBusService, kBusPath, kBusService, QStringLiteral("GetNameOwner"),
         {options_.service}, [this, handler = std::move(handler)](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1 ||
            reply.arguments().first().metaType().id() != QMetaType::QString ||
            reply.arguments().first().toString() != owner_) {
            fail(tr("The USB portal owner changed; device access has stopped."));
            return;
        }
        handler();
    });
}

void Access::deviceEvents(const QDBusObjectPath &session, const Events &events,
                         const QDBusMessage &message) {
    if (!running_ || session.path() != sessionPath_ || message.service() != owner_)
        return;
    if (events.size() > kMaximumDevices) {
        fail(tr("The USB portal returned too many device events."));
        return;
    }
    bool revoked = false;
    bool tooMany = false;
    QStringList revokedIds;
    {
        QMutexLocker lock(&registry_.mutex_);
        for (const Event &event : events) {
            if (!isDeviceId(event.id))
                continue;
            auto existing = registry_.entries_.find(event.id);
            if (event.action == QStringLiteral("remove")) {
                logPortalEvent(QStringLiteral("device_removed"),
                               {{QStringLiteral("known"),
                                 boolText(existing != registry_.entries_.end())},
                                {QStringLiteral("granted"),
                                 boolText(existing != registry_.entries_.end() &&
                                          existing->info.granted)}});
                if (existing != registry_.entries_.end()) {
                    revoked = revoked || existing->info.granted;
                    if (existing->info.granted)
                        revokedIds.append(event.id);
                    attempted_.remove(existing->info.endpoint);
                    registry_.entries_.erase(existing);
                }
                continue;
            }
            if (event.action != QStringLiteral("add") && event.action != QStringLiteral("change"))
                continue;
            const QVariantMap udev = variantMap(event.properties.value(QStringLiteral("properties")));
            const quint16 productId = hexId(udev, QStringLiteral("ID_MODEL_ID"));
            if (hexId(udev, QStringLiteral("ID_VENDOR_ID")) != 0x391a ||
                !printerProductProfileForId(productId)) {
                logPortalEvent(QStringLiteral("device_ignored"),
                               {{QStringLiteral("action"), event.action},
                                {QStringLiteral("vendor_id"),
                                 textProperty(udev, QStringLiteral("ID_VENDOR_ID"))},
                                {QStringLiteral("model_id"),
                                 textProperty(udev, QStringLiteral("ID_MODEL_ID"))}});
                if (existing != registry_.entries_.end()) {
                    revoked = revoked || existing->info.granted;
                    if (existing->info.granted)
                        revokedIds.append(event.id);
                    attempted_.remove(existing->info.endpoint);
                    registry_.entries_.erase(existing);
                }
                continue;
            }
            const QString serial = textProperty(udev, QStringLiteral("ID_SERIAL_SHORT"));
            const bool readable = boolean(event.properties, QStringLiteral("readable"));
            const bool writable = boolean(event.properties, QStringLiteral("writable"));
            const bool sameIdentity = existing != registry_.entries_.end() &&
                existing->info.productId == productId && existing->info.serial == serial;
            Registry::Entry entry;
            if (sameIdentity)
                entry = *existing;
            else {
                if (existing != registry_.entries_.end()) {
                    revoked = revoked || existing->info.granted;
                    if (existing->info.granted)
                        revokedIds.append(event.id);
                    attempted_.remove(existing->info.endpoint);
                }
                entry.info.endpoint = QStringLiteral("portal-usb:") + token();
            }
            if ((!readable || !writable) && entry.info.granted) {
                logPortalEvent(QStringLiteral("grant_revoked"),
                               {{QStringLiteral("reason"), QStringLiteral("access-lost")}});
                revoked = true;
                revokedIds.append(event.id);
                attempted_.remove(entry.info.endpoint);
                entry.info.endpoint = QStringLiteral("portal-usb:") + token();
                entry.descriptor = {};
                entry.info.granted = false;
            }
            entry.info.id = event.id;
            entry.info.productId = productId;
            entry.info.serial = serial;
            entry.info.manufacturer = textProperty(udev, QStringLiteral("ID_VENDOR_FROM_DATABASE"));
            entry.info.product = textProperty(udev, QStringLiteral("ID_MODEL_FROM_DATABASE"));
            entry.info.readable = readable;
            entry.info.writable = writable;
            logPortalEvent(QStringLiteral("device_event"),
                           {{QStringLiteral("action"), event.action},
                            {QStringLiteral("product"), printerProductIdString(productId)},
                            {QStringLiteral("readable"), boolText(readable)},
                            {QStringLiteral("writable"), boolText(writable)},
                            {QStringLiteral("same_identity"), boolText(sameIdentity)},
                            {QStringLiteral("granted"), boolText(entry.info.granted)}});
            registry_.entries_.insert(event.id, std::move(entry));
            if (registry_.entries_.size() > kMaximumDevices) {
                tooMany = true;
                break;
            }
        }
    }
    if (revoked)
        emit endpointRevoked();
    if (tooMany) {
        fail(tr("The USB portal returned too many devices."));
        return;
    }
    releaseDevices(revokedIds);
    if (!maybeAcquire())
        publish();
}

bool Access::maybeAcquire() {
    if (!running_ || !sessionReady_ || !requestPath_.isEmpty() || releasesInFlight_ != 0)
        return false;
    const Snapshot snapshot = registry_.snapshot();
    if (snapshot.devices.size() != 1)
        return false;
    const DeviceInfo device = snapshot.devices.first();
    if (device.granted || !device.readable || !device.writable || attempted_.contains(device.endpoint))
        return false;
    attempted_.insert(device.endpoint);
    pendingId_ = device.id;
    pendingEndpoint_ = device.endpoint;
    pendingDescriptor_ = {};
    resultSeen_ = false;
    acquisitionAcknowledged_ = false;
    earlyResponse_.reset();
    finishCalls_ = 0;
    const QString handleToken = token();
    requestPath_ = objectPath(QStringLiteral("request"), bus_, handleToken);
    if (!bus_.connect(owner_, requestPath_, kRequestInterface, QStringLiteral("Response"),
                      this, SLOT(acquisitionResponse(uint,QVariantMap,QDBusMessage)))) {
        fail(tr("Cannot subscribe to the USB permission response."));
        // fail() already published the terminal state.
        return true;
    }
    permissionDeadline_.start(qMax(1, options_.permissionTimeoutMs));
    {
        QMutexLocker lock(&registry_.mutex_);
        // A new open request supersedes an earlier denial text.
        registry_.acquisitionPending_ = true;
        registry_.error_.clear();
    }
    logPortalEvent(QStringLiteral("acquire_requested"),
                   {{QStringLiteral("product"), printerProductIdString(device.productId)},
                    {QStringLiteral("timeout_ms"),
                     QString::number(qMax(1, options_.permissionTimeoutMs))}});
    const QString request = requestPath_;
    call(owner_, kDesktopPath, kUsbInterface, QStringLiteral("AcquireDevices"),
         {QString(), QVariant::fromValue(Devices{{device.id, {{QStringLiteral("writable"), true}}}}),
          QVariantMap{{QStringLiteral("handle_token"), handleToken}}},
         [this, request](const QDBusMessage &reply) {
        if (requestPath_ != request)
            return;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            fail(tr("Cannot request USB access: %1").arg(reply.errorMessage()));
            return;
        }
        if (reply.arguments().size() != 1 ||
            qdbus_cast<QDBusObjectPath>(reply.arguments().first()).path() != request) {
            fail(tr("The USB portal returned an unexpected permission request."));
            return;
        }
        acquisitionAcknowledged_ = true;
        logPortalEvent(QStringLiteral("acquire_acknowledged"));
        if (earlyResponse_) {
            const uint response = *earlyResponse_;
            earlyResponse_.reset();
            processAcquisitionResponse(response);
        }
    });
    // Let the discovery status report the open request while the host
    // dialog is pending. Callers skip their own publish() for this path.
    publish();
    return true;
}

void Access::acquisitionResponse(uint response, const QVariantMap &,
                                const QDBusMessage &message) {
    if (!running_ || requestPath_.isEmpty() || message.path() != requestPath_ ||
        message.service() != owner_)
        return;
    bus_.disconnect(owner_, requestPath_, kRequestInterface, QStringLiteral("Response"),
                    this, SLOT(acquisitionResponse(uint,QVariantMap,QDBusMessage)));
    if (!acquisitionAcknowledged_) {
        earlyResponse_ = response;
        return;
    }
    processAcquisitionResponse(response);
}

void Access::processAcquisitionResponse(uint response) {
    logPortalEvent(QStringLiteral("acquire_response"),
                   {{QStringLiteral("code"), QString::number(response)}});
    if (response != 0) {
        permissionDeadline_.stop();
        requestPath_.clear();
        pendingId_.clear();
        pendingEndpoint_.clear();
        {
            QMutexLocker lock(&registry_.mutex_);
            registry_.acquisitionPending_ = false;
            registry_.error_ = tr("USB access was not granted. Allow access and restart the Flatpak background runtime to try again.");
        }
        // A different endpoint may have appeared while this request was open.
        // The attempted-endpoint set still prevents retrying the denied device.
        if (!maybeAcquire())
            publish();
        return;
    }
    finishAcquire();
}

void Access::finishAcquire() {
    if (++finishCalls_ > 4) {
        fail(tr("The USB portal did not finish returning device descriptors."));
        return;
    }
    const QString request = requestPath_;
    call(owner_, kDesktopPath, kUsbInterface, QStringLiteral("FinishAcquireDevices"),
         {QVariant::fromValue(QDBusObjectPath(request)), QVariantMap{}},
         [this, request](const QDBusMessage &reply) {
        if (requestPath_ != request)
            return;
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 2 ||
            reply.arguments().at(1).metaType().id() != QMetaType::Bool) {
            fail(tr("Cannot acquire the USB device: %1").arg(reply.errorMessage()));
            return;
        }
        const Devices results = qdbus_cast<Devices>(reply.arguments().first());
        if (results.size() > 1 || (!results.isEmpty() &&
            (results.first().id != pendingId_ || resultSeen_))) {
            fail(tr("The USB portal returned an unexpected device descriptor."));
            return;
        }
        if (!results.isEmpty()) {
            resultSeen_ = true;
            const QVariantMap properties = results.first().properties;
            const auto descriptor = qdbus_cast<QDBusUnixFileDescriptor>(
                properties.value(QStringLiteral("fd")));
            if (!boolean(properties, QStringLiteral("success")) || !usableDescriptor(descriptor)) {
                fail(tr("The USB portal did not return a writable device descriptor: %1")
                         .arg(textProperty(properties, QStringLiteral("error"))));
                return;
            }
            pendingDescriptor_ = descriptor;
        }
        if (!reply.arguments().at(1).toBool()) {
            finishAcquire();
            return;
        }
        if (!resultSeen_) {
            fail(tr("The USB portal omitted the requested device descriptor."));
            return;
        }
        verifyOwner([this, request]() {
            if (requestPath_ != request)
                return;
            bool accepted = false;
            {
                QMutexLocker lock(&registry_.mutex_);
                registry_.acquisitionPending_ = false;
                auto entry = registry_.entries_.find(pendingId_);
                if (entry != registry_.entries_.end() && entry->info.endpoint == pendingEndpoint_ &&
                    entry->info.readable && entry->info.writable) {
                    entry->descriptor = pendingDescriptor_;
                    entry->info.granted = true;
                    registry_.error_.clear();
                    accepted = true;
                }
            }
            logPortalEvent(accepted ? QStringLiteral("acquire_granted")
                                    : QStringLiteral("acquire_discarded"));
            if (!accepted)
                releaseDevices({pendingId_});
            permissionDeadline_.stop();
            requestPath_.clear();
            pendingId_.clear();
            pendingEndpoint_.clear();
            pendingDescriptor_ = {};
            if (!maybeAcquire())
                publish();
        });
    });
}

void Access::releaseDevices(const QStringList &ids) {
    if (ids.isEmpty() || !running_)
        return;
    logPortalEvent(QStringLiteral("release_requested"),
                   {{QStringLiteral("count"), QString::number(ids.size())}});
    ++releasesInFlight_;
    call(owner_, kDesktopPath, kUsbInterface, QStringLiteral("ReleaseDevices"),
         {ids, QVariantMap{}}, [this](const QDBusMessage &reply) {
        --releasesInFlight_;
        if (reply.type() != QDBusMessage::ReplyMessage || !reply.arguments().isEmpty()) {
            fail(tr("The USB portal could not release an obsolete device grant."));
            return;
        }
        maybeAcquire();
    });
}

void Access::publish() { emit changed(); }

void Access::closeRemote(const QString &path, const QString &interface) {
    if (path.isEmpty() || owner_.isEmpty() || !bus_.isConnected())
        return;
    QDBusMessage message = QDBusMessage::createMethodCall(
        owner_, path, interface, QStringLiteral("Close"));
    message.setAutoStartService(false);
    bus_.call(message, QDBus::NoBlock);
}

void Access::sessionClosed(const QVariantMap &, const QDBusMessage &message) {
    if (running_ && message.service() == owner_ && message.path() == sessionPath_)
        fail(tr("The USB portal session closed; device access has stopped."));
}

void Access::fail(const QString &message) {
    logPortalEvent(QStringLiteral("failed"), {{QStringLiteral("message"), message}});
    stop();
    {
        QMutexLocker lock(&registry_.mutex_);
        registry_.error_ = message;
    }
    publish();
    emit errorOccurred(message);
}

void Access::stop() {
    if (!running_ && connectionName_.isEmpty())
        return;
    logPortalEvent(QStringLiteral("stopped"));
    running_ = false;
    ++generation_;
    sessionReady_ = false;
    permissionDeadline_.stop();
    connectionCheck_.stop();
    closeRemote(requestPath_, kRequestInterface);
    closeRemote(sessionPath_, kSessionInterface);
    if (watcher_) {
        watcher_->disconnect(this);
        watcher_->deleteLater();
    }
    watcher_ = nullptr;
    // A dedicated connection owns every USB grant; disconnect releases them,
    // including partial or late acquisitions, without affecting the runtime API.
    QDBusConnection::disconnectFromBus(connectionName_);
    bus_ = QDBusConnection(QString());
    connectionName_.clear();
    owner_.clear();
    sessionPath_.clear();
    requestPath_.clear();
    pendingId_.clear();
    pendingEndpoint_.clear();
    pendingDescriptor_ = {};
    earlyResponse_.reset();
    releasesInFlight_ = 0;
    attempted_.clear();
    bool hadGrant = false;
    {
        QMutexLocker lock(&registry_.mutex_);
        for (const auto &entry : registry_.entries_)
            hadGrant = hadGrant || entry.info.granted;
        registry_.entries_.clear();
        registry_.monitoring_ = false;
        registry_.acquisitionPending_ = false;
    }
    if (hadGrant)
        emit endpointRevoked();
    publish();
}
}  // namespace tryx::portal_usb
