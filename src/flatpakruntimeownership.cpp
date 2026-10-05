#include "flatpakruntimeownership.h"
#include "packagingcontext.h"

#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QUuid>

namespace {
bool claim(const QDBusConnection &bus, const QString &name) {
    if (!bus.isConnected() || !bus.interface())
        return false;
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        bus.interface()->registerService(
            name, QDBusConnectionInterface::DontQueueService,
            QDBusConnectionInterface::DontAllowReplacement);
    return reply.isValid() && reply.value() == QDBusConnectionInterface::ServiceRegistered;
}
}

FlatpakRuntimeOwnership::FlatpakRuntimeOwnership(
    const QDBusConnection &bus, QObject *parent)
    : QObject(parent), apiBus_(bus), guardBus_(QString()) {
    connectionCheck_.setInterval(250);
    connect(&connectionCheck_, &QTimer::timeout, this, [this]() {
        if (held_ && (!apiBus_.isConnected() || !guardBus_.isConnected()))
            invalidate();
    });
}

FlatpakRuntimeOwnership::~FlatpakRuntimeOwnership() { release(); }

bool FlatpakRuntimeOwnership::acquire(QString *error) {
    using namespace tryx::packaging;
    if (held_)
        return true;
    release();
    guardConnectionName_ = QStringLiteral("tryx-flatpak-exclusion-") +
        QUuid::createUuid().toString(QUuid::Id128);
    guardBus_ = QDBusConnection::connectToBus(
        QDBusConnection::SessionBus, guardConnectionName_);
    if (guardBus_.baseService() == apiBus_.baseService() ||
        !claim(guardBus_, nativeRuntimeService())) {
        if (error)
            *error = tr("Another TRYX runtime is running or the global D-Bus exclusion is unavailable. "
                        "Finish its work and stop it before starting the Flatpak.");
        release();
        return false;
    }
    guardClaimed_ = true;
    if (!claim(apiBus_, flatpakRuntimeService())) {
        if (error)
            *error = tr("Another Flatpak runtime already owns the application D-Bus name.");
        release();
        return false;
    }
    apiClaimed_ = true;
    // Observe each owner from the OTHER connection so a connection loss is
    // visible even when the lost connection itself cannot deliver signals.
    guardWatcher_ = new QDBusServiceWatcher(
        nativeRuntimeService(), apiBus_, QDBusServiceWatcher::WatchForOwnerChange, this);
    apiWatcher_ = new QDBusServiceWatcher(
        flatpakRuntimeService(), guardBus_, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(guardWatcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &current) {
        if (held_ && current != guardBus_.baseService())
            invalidate();
    });
    connect(apiWatcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &current) {
        if (held_ && current != apiBus_.baseService())
            invalidate();
    });
    held_ = true;
    connectionCheck_.start();
    return true;
}

void FlatpakRuntimeOwnership::invalidate() {
    if (!held_)
        return;
    stopServing();
    emit ownershipLost();
}

void FlatpakRuntimeOwnership::stopServing() {
    held_ = false;
    connectionCheck_.stop();
    if (apiClaimed_) {
        // Block calls to the pinned unique owner as well as the well-known name.
        apiBus_.unregisterObject(QStringLiteral("/org/tryx/Panorama"),
                                 QDBusConnection::UnregisterTree);
        apiBus_.unregisterService(tryx::packaging::flatpakRuntimeService());
        apiClaimed_ = false;
    }
    // The global exclusion intentionally remains held during worker teardown.
}

void FlatpakRuntimeOwnership::release() {
    stopServing();
    delete apiWatcher_;
    apiWatcher_ = nullptr;
    delete guardWatcher_;
    guardWatcher_ = nullptr;
    if (guardClaimed_) {
        guardBus_.unregisterService(tryx::packaging::nativeRuntimeService());
        guardClaimed_ = false;
    }
    if (!guardConnectionName_.isEmpty())
        QDBusConnection::disconnectFromBus(guardConnectionName_);
    guardBus_ = QDBusConnection(QString());
    guardConnectionName_.clear();
}
