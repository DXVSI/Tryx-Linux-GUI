#include "flatpakruntimeownership.h"
#include "packagingcontext.h"

#include <QtTest>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QUuid>

namespace {
using namespace tryx::packaging;
const QString apiPath = QStringLiteral("/org/tryx/Panorama");
const QString apiInterface = QStringLiteral("org.tryx.Panorama.Manager2");

struct Connection {
    QString name = QUuid::createUuid().toString(QUuid::Id128);
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
    ~Connection() { QDBusConnection::disconnectFromBus(name); }
};

QString owner(const QDBusConnection &bus, const QString &service) {
    const QDBusReply<QString> reply = bus.interface()->serviceOwner(service);
    return reply.isValid() ? reply.value() : QString();
}

QDBusMessage callApi(const QDBusConnection &bus, const QString &destination) {
    auto call = QDBusMessage::createMethodCall(destination, apiPath, apiInterface,
                                              QStringLiteral("GetRuntimeApiVersion"));
    call.setAutoStartService(false);
    return bus.call(call, QDBus::BlockWithGui, 500);
}
}

class FakeApi final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")
public slots:
    uint GetRuntimeApiVersion() { return 42; }
};

class FlatpakRuntimeOwnershipTests final : public QObject {
    Q_OBJECT
private slots:
    void nativeOwnerBlocksSandboxWithoutQueuing() {
        Connection native, api;
        QVERIFY(native.bus.registerService(nativeRuntimeService()));
        FlatpakRuntimeOwnership ownership(api.bus);
        QString error;
        QVERIFY(!ownership.acquire(&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(owner(native.bus, nativeRuntimeService()), native.bus.baseService());
        QVERIFY(owner(api.bus, flatpakRuntimeService()).isEmpty());
        native.bus.unregisterService(nativeRuntimeService());
        QTest::qWait(50);
        QVERIFY(owner(api.bus, nativeRuntimeService()).isEmpty());
    }

    void nativeClientsCannotReachSandboxApiThroughSentinel() {
        Connection api, client;
        FlatpakRuntimeOwnership ownership(api.bus);
        QString error;
        QVERIFY2(ownership.acquire(&error), qPrintable(error));
        FakeApi object;
        QVERIFY(api.bus.registerObject(apiPath, &object, QDBusConnection::ExportAllSlots));
        const QString sentinel = owner(client.bus, nativeRuntimeService());
        QVERIFY(!sentinel.isEmpty());
        QVERIFY(sentinel != api.bus.baseService());
        QCOMPARE(owner(client.bus, flatpakRuntimeService()), api.bus.baseService());
        QCOMPARE(callApi(client.bus, sentinel).type(), QDBusMessage::ErrorMessage);
        const auto reply = callApi(client.bus, flatpakRuntimeService());
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.arguments().first().toUInt(), 42U);
        QVERIFY(!client.bus.registerService(nativeRuntimeService()));
    }

    void secondSandboxCannotStealOrReleaseFirstOwner() {
        Connection first, second;
        FlatpakRuntimeOwnership winner(first.bus);
        QString error;
        QVERIFY2(winner.acquire(&error), qPrintable(error));
        const QString sentinel = owner(first.bus, nativeRuntimeService());
        {
            FlatpakRuntimeOwnership loser(second.bus);
            QVERIFY(!loser.acquire(&error));
        }
        QCOMPARE(owner(first.bus, nativeRuntimeService()), sentinel);
        QCOMPARE(owner(first.bus, flatpakRuntimeService()), first.bus.baseService());
    }

    void apiConflictReleasesOnlyOurSentinel() {
        Connection foreign, api;
        QVERIFY(foreign.bus.registerService(flatpakRuntimeService()));
        FlatpakRuntimeOwnership ownership(api.bus);
        QString error;
        QVERIFY(!ownership.acquire(&error));
        QVERIFY(owner(api.bus, nativeRuntimeService()).isEmpty());
        QCOMPARE(owner(api.bus, flatpakRuntimeService()), foreign.bus.baseService());
    }

    void sentinelLivesUntilAfterApiAndWorkerTeardown() {
        Connection api, contender;
        {
            FlatpakRuntimeOwnership ownership(api.bus);
            QString error;
            QVERIFY2(ownership.acquire(&error), qPrintable(error));
            FakeApi object;
            QVERIFY(api.bus.registerObject(apiPath, &object, QDBusConnection::ExportAllSlots));
            ownership.stopServing();
            QCOMPARE(callApi(contender.bus, api.bus.baseService()).type(), QDBusMessage::ErrorMessage);
            QVERIFY(owner(contender.bus, flatpakRuntimeService()).isEmpty());
            // Simulate the still-live DeviceManager destructor after aboutToQuit.
            QVERIFY(!contender.bus.registerService(nativeRuntimeService()));
        }
        QVERIFY(contender.bus.registerService(nativeRuntimeService()));
    }

    void guardLossStopsServingWithoutReacquiring() {
        Connection api, client;
        FlatpakRuntimeOwnership ownership(api.bus);
        QString error;
        QVERIFY2(ownership.acquire(&error), qPrintable(error));
        FakeApi object;
        QVERIFY(api.bus.registerObject(apiPath, &object, QDBusConnection::ExportAllSlots));
        QSignalSpy lost(&ownership, &FlatpakRuntimeOwnership::ownershipLost);
        QVERIFY(ownership.guardBus_.unregisterService(nativeRuntimeService()));
        QTRY_COMPARE(lost.count(), 1);
        QCOMPARE(callApi(client.bus, api.bus.baseService()).type(), QDBusMessage::ErrorMessage);
        QVERIFY(owner(client.bus, flatpakRuntimeService()).isEmpty());
        QVERIFY(client.bus.registerService(nativeRuntimeService()));
        QTest::qWait(50);
        QCOMPARE(owner(client.bus, nativeRuntimeService()), client.bus.baseService());
    }

    void apiLossRetainsGuardUntilTeardown() {
        Connection api, client;
        FlatpakRuntimeOwnership ownership(api.bus);
        QString error;
        QVERIFY2(ownership.acquire(&error), qPrintable(error));
        QSignalSpy lost(&ownership, &FlatpakRuntimeOwnership::ownershipLost);
        QVERIFY(api.bus.unregisterService(flatpakRuntimeService()));
        QTRY_COMPARE(lost.count(), 1);
        QVERIFY(!client.bus.registerService(nativeRuntimeService()));
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariable("TRYX_FLATPAK_TEST_ISOLATED") != QStringLiteral("1"))
        return 2;
    QCoreApplication app(argc, argv);
    FlatpakRuntimeOwnershipTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "runtimeownership_tests.moc"
