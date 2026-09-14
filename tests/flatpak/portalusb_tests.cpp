#include "portalusb.h"
#include "usbprintertransport.h"

#include <QtTest>
#include <QDBusContext>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QUuid>

#include <fcntl.h>
#include <unistd.h>

using namespace tryx::portal_usb;

namespace {
const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath = QStringLiteral("/org/freedesktop/portal/desktop");

QString senderPart(const QString &sender) {
    QString part = sender.mid(1);
    return part.replace(QLatin1Char('.'), QLatin1Char('_'));
}

QVariantMap properties(const QString &vendor = QStringLiteral("391a"),
                       const QString &product = QStringLiteral("1021")) {
    return {{QStringLiteral("readable"), true},
            {QStringLiteral("writable"), true},
            {QStringLiteral("properties"), QVariantMap{
                 {QStringLiteral("ID_VENDOR_ID"), vendor},
                 {QStringLiteral("ID_MODEL_ID"), product},
                 {QStringLiteral("ID_SERIAL_SHORT"), QStringLiteral("synthetic-no-usb")}}}};
}
}

class FakeRequest final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    using QObject::QObject;
    bool closed = false;
public slots:
    void Close() { closed = true; }
signals:
    void Response(uint response, const QVariantMap &results);
};

class FakeSession final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public:
    using QObject::QObject;
public slots:
    void Close() {}
signals:
    void Closed(const QVariantMap &details);
};

class FakePortal final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Usb")
public:
    QDBusConnection bus = QDBusConnection::sessionBus();
    Devices devices{{QStringLiteral("device-1"), properties()}};
    Devices lastAcquisition;
    QString sessionPath;
    QString requestPath;
    uint response = 0;
    bool replyAutomatically = true;
    bool invalidDescriptor = false;
    bool foreignResult = false;
    bool earlyResponse = false;
    bool wrongRequestPath = false;
    bool multipart = false;
    bool neverFinish = false;
    int acquisitions = 0;
    int finishes = 0;
    int releases = 0;
    QStringList releasedIds;
    FakeRequest request;
    FakeSession session;

    ~FakePortal() override {
        bus.unregisterObject(portalPath, QDBusConnection::UnregisterTree);
        bus.unregisterService(portalService);
    }
    bool install() {
        return bus.registerService(portalService) &&
            bus.registerObject(portalPath, this, QDBusConnection::ExportAllSlots |
                               QDBusConnection::ExportAllSignals);
    }
    void sendEvent(const QString &action, const Device &device) {
        emit DeviceEvents(QDBusObjectPath(sessionPath),
                          {{action, device.id, device.properties}});
    }

public slots:
    QDBusObjectPath CreateSession(const QVariantMap &options) {
        sessionPath = portalPath + QStringLiteral("/session/") +
            senderPart(message().service()) + QLatin1Char('/') +
            options.value(QStringLiteral("session_handle_token")).toString();
        bus.registerObject(sessionPath, &session, QDBusConnection::ExportAllSlots |
                           QDBusConnection::ExportAllSignals);
        // Exercise the subscribe-before-call requirement.
        Events initial;
        for (const Device &device : devices)
            initial.append({QStringLiteral("add"), device.id, device.properties});
        emit DeviceEvents(QDBusObjectPath(sessionPath), initial);
        return QDBusObjectPath(sessionPath);
    }
    QDBusObjectPath AcquireDevices(const QString &, const Devices &requested,
                                   const QVariantMap &options) {
        ++acquisitions;
        lastAcquisition = requested;
        requestPath = portalPath + QStringLiteral("/request/") +
            senderPart(message().service()) + QLatin1Char('/') +
            options.value(QStringLiteral("handle_token")).toString();
        bus.registerObject(requestPath, &request, QDBusConnection::ExportAllSlots |
                           QDBusConnection::ExportAllSignals);
        if (replyAutomatically) {
            if (earlyResponse)
                emit request.Response(response, {});
            else
                QTimer::singleShot(0, &request, [this]() { emit request.Response(response, {}); });
        }
        return QDBusObjectPath(wrongRequestPath ? requestPath + "_wrong" : requestPath);
    }
    void FinishAcquireDevices(const QDBusObjectPath &, const QVariantMap &,
                              Devices &results, bool &finished) {
        ++finishes;
        if (finishes > 1 && (multipart || neverFinish)) {
            results.clear();
            finished = !neverFinish;
            return;
        }
        const int descriptor = ::open(invalidDescriptor ? "/dev/zero" : "/dev/null",
                                      (invalidDescriptor ? O_RDONLY : O_RDWR) | O_CLOEXEC);
        QDBusUnixFileDescriptor handle(descriptor);
        ::close(descriptor);
        results = {{foreignResult ? QStringLiteral("foreign-device") : lastAcquisition.first().id,
                    {{QStringLiteral("success"), true},
                     {QStringLiteral("fd"), QVariant::fromValue(handle)}}}};
        finished = !multipart && !neverFinish;
    }
    void ReleaseDevices(const QStringList &ids, const QVariantMap &) {
        ++releases;
        releasedIds.append(ids);
    }
signals:
    void DeviceEvents(const QDBusObjectPath &session, const Events &events);
};

class PortalUsbTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { registerTypes(); }

    void grantsOnlyExactSupportedProductAndClosesOnStop() {
        FakePortal portal;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.acquisitions, 1);
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        const auto device = registry.snapshot().devices.first();
        QCOMPARE(portal.lastAcquisition.first().id, QStringLiteral("device-1"));
        QCOMPARE(portal.lastAcquisition.first().properties.value("writable").toBool(), true);
        QVERIFY(registry.descriptorFor(device.endpoint, 0x1021).isValid());
        QVERIFY(!registry.descriptorFor(device.endpoint, 0x1011).isValid());
        access.stop();
        QVERIFY(!registry.hasGrant(device.endpoint, 0x1021));
        QVERIFY(!registry.snapshot().monitoring);
    }

    void denialDoesNotPromptOnEveryChange() {
        FakePortal portal;
        portal.response = 1;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.acquisitions, 1);
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        portal.sendEvent(QStringLiteral("change"), portal.devices.first());
        QTest::qWait(50);
        QCOMPARE(portal.acquisitions, 1);
        QCOMPARE(portal.finishes, 0);
        QVERIFY(!registry.snapshot().devices.first().granted);
    }

    void removalRevokesTheLeaseAndLateGrantCannotResurrectIt() {
        FakePortal portal;
        portal.replyAutomatically = false;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.acquisitions, 1);
        const QString oldEndpoint = registry.snapshot().devices.first().endpoint;
        portal.sendEvent(QStringLiteral("remove"), portal.devices.first());
        QTRY_VERIFY(registry.snapshot().devices.isEmpty());
        emit portal.request.Response(0, {});
        QTest::qWait(50);
        QVERIFY(!registry.hasGrant(oldEndpoint, 0x1021));
        QVERIFY(registry.snapshot().devices.isEmpty());
    }

    void removalOfGrantedDeviceInvalidatesEndpoint() {
        FakePortal portal;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QSignalSpy revoked(&access, &Access::endpointRevoked);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        const QString endpoint = registry.snapshot().devices.first().endpoint;
        portal.sendEvent(QStringLiteral("remove"), portal.devices.first());
        QTRY_COMPARE(revoked.count(), 1);
        QVERIFY(!registry.hasGrant(endpoint, 0x1021));
    }

    void unsupportedDevicesAndAmbiguityDoNotAcquire_data() {
        QTest::addColumn<Devices>("devices");
        QTest::newRow("other-vendor") << Devices{{"wrong", properties("1234", "1021")}};
        QTest::newRow("other-model") << Devices{{"wrong", properties("391a", "ffff")}};
        QTest::newRow("malformed-id") << Devices{{"wrong", properties("391a", "1021garbage")}};
        QTest::newRow("two-devices") << Devices{{"one", properties()}, {"two", properties()}};
        auto readOnly = properties();
        readOnly.insert("writable", false);
        QTest::newRow("host-read-only") << Devices{{"read-only", readOnly}};
    }
    void unsupportedDevicesAndAmbiguityDoNotAcquire() {
        QFETCH(Devices, devices);
        FakePortal portal;
        portal.devices = devices;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().monitoring);
        QTest::qWait(50);
        QCOMPARE(portal.acquisitions, 0);
    }

    void malformedAcquisitionFailsClosed_data() {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("read-only-fd") << false;
        QTest::newRow("unexpected-device") << true;
    }
    void malformedAcquisitionFailsClosed() {
        QFETCH(bool, foreign);
        FakePortal portal;
        portal.foreignResult = foreign;
        portal.invalidDescriptor = !foreign;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.finishes, 1);
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        for (const auto &device : registry.snapshot().devices)
            QVERIFY(!device.granted);
    }

    void missingPortalIsExplicitlyUnavailable() {
        Registry registry;
        Access::Options options;
        options.service = QStringLiteral("org.tryx.NonexistentUsbPortal");
        Access access(registry, options);
        QVERIFY(access.start());
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        QVERIFY(!registry.snapshot().monitoring);
    }

    void wrongAcknowledgementCannotUseEarlySuccess() {
        FakePortal portal;
        portal.earlyResponse = true;
        portal.wrongRequestPath = true;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        QCOMPARE(portal.finishes, 0);
        QVERIFY(!registry.snapshot().monitoring);
    }

    void finishesAllDescriptorPagesBeforeGranting() {
        FakePortal portal;
        portal.multipart = true;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        QCOMPARE(portal.finishes, 2);
    }

    void incompleteDescriptorSequenceIsBounded() {
        FakePortal portal;
        portal.neverFinish = true;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        QCOMPARE(portal.finishes, 4);
        QVERIFY(!registry.snapshot().monitoring);
    }

    void permissionTimeoutClosesRequestAndIgnoresLateSuccess() {
        FakePortal portal;
        portal.replyAutomatically = false;
        QVERIFY(portal.install());
        Registry registry;
        Access::Options options;
        options.permissionTimeoutMs = 100;
        Access access(registry, options);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.acquisitions, 1);
        QTRY_VERIFY(!registry.snapshot().error.isEmpty());
        QTRY_VERIFY(portal.request.closed);
        emit portal.request.Response(0, {});
        QTest::qWait(25);
        QCOMPARE(portal.finishes, 0);
        QVERIFY(!registry.snapshot().monitoring);
    }

    void portalOwnerLossRevokesWithoutReacquiring() {
        FakePortal portal;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        QVERIFY(portal.bus.unregisterService(portalService));
        QTRY_VERIFY(!registry.snapshot().monitoring);
        QVERIFY(!registry.snapshot().error.isEmpty());
        QCOMPARE(portal.acquisitions, 1);
    }

    void deniedRemovedDeviceDoesNotStrandItsReplacement() {
        FakePortal portal;
        portal.replyAutomatically = false;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_COMPARE(portal.acquisitions, 1);
        portal.sendEvent("remove", portal.devices.first());
        const Device replacement{"replacement", properties()};
        portal.sendEvent("add", replacement);
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().id == replacement.id);
        QCOMPARE(portal.acquisitions, 1);
        emit portal.request.Response(1, {});
        QTRY_COMPARE_WITH_TIMEOUT(portal.acquisitions, 2, 1000);
        QCOMPARE(portal.lastAcquisition.first().id, replacement.id);
        emit portal.request.Response(1, {});
        QTest::qWait(50);
        QCOMPARE(portal.acquisitions, 2);
    }

    void realTransportRejectsNonUsbDescriptorWithoutLeakingIt() {
        FakePortal portal;
        QVERIFY(portal.install());
        Access access(Registry::instance());
        QVERIFY(access.start());
        QTRY_VERIFY(Registry::instance().snapshot().devices.size() == 1 &&
                    Registry::instance().snapshot().devices.first().granted);
        const auto device = Registry::instance().snapshot().devices.first();
        const auto descriptorCount = []() {
            return QDir(QStringLiteral("/proc/self/fd")).entryList(
                QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).size();
        };
        // Constructing the real backend uses NO_DEVICE_DISCOVERY. The only
        // provided FD is /dev/null, which must fail before any interface claim.
        UsbPrinterTransport transport;
        const auto count = descriptorCount();
        QString error;
        QVERIFY(!transport.open(device.endpoint, 0x1011, &error));
        for (int attempt = 0; attempt < 5; ++attempt) {
            QVERIFY(!transport.open(device.endpoint, device.productId, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!transport.isOpenFor(device.endpoint, device.productId));
            QCOMPARE(descriptorCount(), count);
        }
        access.stop();
        const auto discovery = PrinterProtocol::discover();
        QVERIFY(discovery.blocksLegacyTransport());
        QCOMPARE(discovery.state, PrinterProtocol::DiscoveryState::MonitoringUnavailable);
    }

    void revocationDrainsDeferredCancellationWithoutNewSubmissions() {
        FakePortal portal;
        QVERIFY(portal.install());
        Access access(Registry::instance());
        QVERIFY(access.start());
        QTRY_VERIFY(Registry::instance().snapshot().devices.size() == 1 &&
                    Registry::instance().snapshot().devices.first().granted);
        const auto device = Registry::instance().snapshot().devices.first();
        int probes = 0;
        const auto result = UsbPrinterTransport::runScenarioForTesting(
            {}, QByteArrayLiteral("synthetic OUT"), 1000, 100, device.endpoint,
            device.productId, [&]() {
                if (++probes == 2)
                    access.stop(); // After submit, without cancelling the operation context.
                return false;
            });
        QVERIFY(!result.writeSucceeded);
        QVERIFY(result.writeCompletionKnown);
        QVERIFY(result.writeCancelled);
        QVERIFY(!result.error.contains(QStringLiteral("did not complete")));
        QCOMPARE(result.inputSubmissions, 1);
        QCOMPARE(result.outputSubmissions, 1);
        QCOMPARE(result.inputRearmsDuringOutput, 0);
    }

    void identityChangeReleasesOldGrantAndAllowsOneNewAttempt() {
        FakePortal portal;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        const QString endpoint = registry.snapshot().devices.first().endpoint;
        auto changed = portal.devices.first();
        auto udev = changed.properties.value("properties").toMap();
        udev.insert("ID_SERIAL_SHORT", "new-device-identity");
        changed.properties.insert("properties", udev);
        portal.sendEvent("change", changed);
        QTRY_VERIFY_WITH_TIMEOUT(!registry.hasGrant(endpoint, 0x1021), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(portal.releases, 1, 1000);
        QCOMPARE(portal.releasedIds, QStringList{"device-1"});
        QTRY_COMPARE_WITH_TIMEOUT(portal.acquisitions, 2, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(registry.snapshot().devices.first().granted, 1000);
        QVERIFY(registry.snapshot().devices.first().endpoint != endpoint);
        portal.sendEvent("change", changed);
        QTest::qWait(50);
        QCOMPARE(portal.acquisitions, 2);
    }

    void readOnlyChangeReleasesGrantAndInvalidatesOldEndpoint() {
        FakePortal portal;
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().devices.size() == 1 &&
                    registry.snapshot().devices.first().granted);
        const QString endpoint = registry.snapshot().devices.first().endpoint;
        auto changed = portal.devices.first();
        changed.properties.insert("writable", false);
        portal.sendEvent("change", changed);
        QTRY_VERIFY_WITH_TIMEOUT(!registry.hasGrant(endpoint, 0x1021), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(portal.releases, 1, 1000);
        QCOMPARE(portal.releasedIds, QStringList{"device-1"});
        QCOMPARE(portal.acquisitions, 1);
    }

    void staleSessionClosedCannotStopNewSession() {
        FakePortal portal;
        portal.devices.clear();
        QVERIFY(portal.install());
        Registry registry;
        Access access(registry);
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().monitoring);
        const QString oldPath = portal.sessionPath;
        access.stop();
        QVERIFY(access.start());
        QTRY_VERIFY(registry.snapshot().monitoring);
        QVERIFY(portal.sessionPath != oldPath);
        auto closed = QDBusMessage::createSignal(
            oldPath, "org.freedesktop.portal.Session", "Closed");
        closed.setArguments({QVariantMap{}});
        QVERIFY(portal.bus.send(closed));
        QTest::qWait(50);
        QVERIFY(registry.snapshot().monitoring);
        // Also exercise an already queued delivery from the old connection.
        // Such deliveries may survive disconnect(), unlike a new bus signal.
        const auto queuedClosed = QDBusMessage::createMethodCall(
            portal.bus.baseService(), oldPath, "org.freedesktop.portal.Session", "Closed");
        QVERIFY(QMetaObject::invokeMethod(
            &access, "sessionClosed", Qt::QueuedConnection,
            Q_ARG(QVariantMap, QVariantMap{}), Q_ARG(QDBusMessage, queuedClosed)));
        QCoreApplication::sendPostedEvents(&access);
        QVERIFY(registry.snapshot().monitoring);
        closed = QDBusMessage::createSignal(
            portal.sessionPath, "org.freedesktop.portal.Session", "Closed");
        closed.setArguments({QVariantMap{}});
        QVERIFY(portal.bus.send(closed));
        QTRY_VERIFY(!registry.snapshot().monitoring);
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariable("TRYX_FLATPAK_TEST_ISOLATED") != QStringLiteral("1"))
        return 2;
    QCoreApplication app(argc, argv);
    PortalUsbTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "portalusb_tests.moc"
