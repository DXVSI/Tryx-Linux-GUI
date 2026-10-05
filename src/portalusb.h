#pragma once

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <optional>

class QDBusServiceWatcher;

namespace tryx::portal_usb {

// The portal wire types are a(sa{sv}) and a(ssa{sv}), not QVariantList.
struct Device {
    QString id;
    QVariantMap properties;
};
using Devices = QList<Device>;
struct Event {
    QString action;
    QString id;
    QVariantMap properties;
};
using Events = QList<Event>;
QDBusArgument &operator<<(QDBusArgument &argument, const Device &device);
const QDBusArgument &operator>>(const QDBusArgument &argument, Device &device);
QDBusArgument &operator<<(QDBusArgument &argument, const Event &event);
const QDBusArgument &operator>>(const QDBusArgument &argument, Event &event);
void registerTypes();

struct DeviceInfo {
    QString id;
    QString endpoint;
    quint16 productId = 0;
    QString manufacturer;
    QString product;
    QString serial;
    bool readable = false;
    bool writable = false;
    bool granted = false;
};

struct Snapshot {
    bool monitoring = false;
    QList<DeviceInfo> devices;
    QString error;
    // An AcquireDevices request is open and the desktop portal has not
    // answered yet; the host dialog should be visible to the user.
    bool acquisitionPending = false;
};

// Cross-thread lease registry. Only the portal owner publishes; worker threads
// can obtain a descriptor for a currently granted, exact product/endpoint.
class Registry final {
public:
    static Registry &instance();
    Snapshot snapshot() const;
    QDBusUnixFileDescriptor descriptorFor(
        const QString &endpoint, quint16 productId) const;
    bool hasGrant(const QString &endpoint, quint16 productId) const;

private:
    friend class Access;
    struct Entry {
        DeviceInfo info;
        QDBusUnixFileDescriptor descriptor;
    };
    mutable QMutex mutex_;
    bool monitoring_ = false;
    bool acquisitionPending_ = false;
    QString error_;
    QHash<QString, Entry> entries_;
};

class Access final : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString service = QStringLiteral("org.freedesktop.portal.Desktop");
        int callTimeoutMs = 2000;
        int permissionTimeoutMs = 120000;
    };
    explicit Access(Registry &registry, QObject *parent = nullptr);
    Access(Registry &registry, const Options &options, QObject *parent = nullptr);
    ~Access() override;
    bool start();
    void stop();

signals:
    void changed();
    void endpointRevoked();
    void errorOccurred(const QString &message);

private slots:
    void deviceEvents(const QDBusObjectPath &session, const Events &events,
                      const QDBusMessage &message);
    void acquisitionResponse(uint response, const QVariantMap &results,
                             const QDBusMessage &message);
    void sessionClosed(const QVariantMap &details, const QDBusMessage &message);

private:
    using ReplyHandler = std::function<void(const QDBusMessage &)>;
    void resolveOwner(bool allowActivation);
    void createSession();
    // Returns true when a permission request was sent and published.
    bool maybeAcquire();
    void finishAcquire();
    void verifyOwner(std::function<void()> handler);
    void releaseDevices(const QStringList &ids);
    void processAcquisitionResponse(uint response);
    void publish();
    void fail(const QString &message);
    void call(const QString &destination, const QString &path,
              const QString &interface, const QString &method,
              const QVariantList &arguments, ReplyHandler handler);
    void closeRemote(const QString &path, const QString &interface);
    Registry &registry_;
    Options options_;
    QString connectionName_;
    QDBusConnection bus_;
    QDBusServiceWatcher *watcher_ = nullptr;
    QTimer permissionDeadline_;
    QTimer connectionCheck_;
    QString owner_;
    QString sessionPath_;
    QString requestPath_;
    QString pendingId_;
    QString pendingEndpoint_;
    QDBusUnixFileDescriptor pendingDescriptor_;
    bool resultSeen_ = false;
    bool acquisitionAcknowledged_ = false;
    std::optional<uint> earlyResponse_;
    int releasesInFlight_ = 0;
    QSet<QString> attempted_;
    quint64 generation_ = 0;
    bool running_ = false;
    bool sessionReady_ = false;
    int finishCalls_ = 0;
};

}  // namespace tryx::portal_usb

Q_DECLARE_METATYPE(tryx::portal_usb::Device)
Q_DECLARE_METATYPE(tryx::portal_usb::Devices)
Q_DECLARE_METATYPE(tryx::portal_usb::Event)
Q_DECLARE_METATYPE(tryx::portal_usb::Events)
