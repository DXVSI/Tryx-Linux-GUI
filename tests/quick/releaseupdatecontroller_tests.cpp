#include "releaseinfo.h"
#include "releaseupdatecontroller.h"
#include "linuxtraycontroller.h"

#include <QDateTime>
#include <QDBusContext>
#include <QDBusPendingCallWatcher>
#include <QDBusVirtualObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QtTest>

#include <cstring>
#include <cstdio>

namespace {

QJsonObject releaseObject(const QString &tag = QStringLiteral("v2.3.0")) {
    return {{QStringLiteral("tag_name"), tag},
            {QStringLiteral("draft"), false},
            {QStringLiteral("prerelease"), false},
            {QStringLiteral("published_at"), QStringLiteral("2026-09-06T00:00:00Z")}};
}

QByteArray releaseBody(const QString &tag = QStringLiteral("v2.3.0")) {
    return QJsonDocument(releaseObject(tag)).toJson(QJsonDocument::Compact);
}

// No socket is created: all request/response traffic terminates in this fixture.
class FakeReply : public QNetworkReply {
public:
    explicit FakeReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
    }
    bool aborted = false;
    QByteArray pending;
    qint64 largestRead = 0;
    void abort() override {
        if (isFinished())
            return;
        aborted = true;
        complete(OperationCanceledError);
    }
    void status(int code) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code);
    }
    void header(const QByteArray &name, const QByteArray &value) {
        setRawHeader(name, value);
    }
    void send(const QByteArray &chunk) {
        pending += chunk;
        emit readyRead();
    }
    void complete(NetworkError error = NoError) {
        if (error != NoError) {
            setError(error, QStringLiteral("Fixture error"));
            emit errorOccurred(error);
        }
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return pending.size() + QNetworkReply::bytesAvailable();
    }
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maximum) override {
        largestRead = qMax(largestRead, maximum);
        const qint64 size = qMin(maximum, qint64(pending.size()));
        std::memcpy(data, pending.constData(), size_t(size));
        pending.remove(0, size);
        return size;
    }
};

class FakeNetwork : public QNetworkAccessManager {
public:
    QList<QNetworkRequest> requests;
    QList<Operation> operations;
    QPointer<FakeReply> latest;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoing) override {
        Q_ASSERT(!outgoing);
        requests.append(request);
        operations.append(operation);
        latest = new FakeReply(request, this);
        return latest;
    }
};

class NotificationService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    int calls = 0;
    bool reject = false;
    QString lastBody;
public slots:
    uint Notify(const QString &, uint, const QString &, const QString &,
                const QString &body, const QStringList &, const QVariantMap &, int) {
        ++calls;
        lastBody = body;
        if (reject)
            sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Fixture refusal"));
        return 1;
    }
};

// Separate process: synchronous introspection must not borrow the test thread's
// event loop. Notify itself is deliberately slow too, but always asynchronous.
class SlowNotificationService : public QDBusVirtualObject {
public:
    QString introspect(const QString &) const override {
        QThread::msleep(1000);
        return QStringLiteral("<interface name=\"org.freedesktop.Notifications\">"
            "<method name=\"Notify\"><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"as\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/>"
            "<arg type=\"i\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/>"
            "</method></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (message.member() != QStringLiteral("Notify"))
            return false;
        message.setDelayedReply(true);
        QTimer::singleShot(1000, this, [message, bus] {
            bus.send(message.createReply(QVariant::fromValue(uint(7))));
        });
        return true;
    }
};

} // namespace

class ReleaseUpdateControllerTests : public QObject {
    Q_OBJECT

    qint64 now_ = 0;
    qint64 utc_ = 1788652800000;

    void launch(ReleaseUpdateController &controller) {
        controller.start();
        controller.poll();
    }
    void hour(ReleaseUpdateController &controller) {
        now_ += 60 * 60 * 1000;
        utc_ += 60 * 60 * 1000;
        controller.poll();
    }

private slots:
    void initTestCase() {
        QVERIFY2(qEnvironmentVariable("TRYX_RELEASE_UPDATE_TEST_ISOLATED") == "1",
                 "Run with TRYX_RELEASE_UPDATE_TEST_ISOLATED=1 dbus-run-session -- ...");
    }
    void init() { now_ = 0; utc_ = 1788652800000; }
    void versionParsing_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << "2.10.0" << "2.10.0";
        QTest::newRow("tag") << "v2.10.0" << "2.10.0";
        QTest::newRow("zero") << "0.0.0" << "0.0.0";
        QTest::newRow("max") << "4294967295.0.0" << "4294967295.0.0";
        const QStringList rejected = {
            "", "2.2", "2.2.0.1", "2.2.0-rc1", "2.2.0+dev", "V2.2.0",
            " 2.2.0", "2.2.0\n", "02.2.0", "2.-1.0", "2.+1.0", "2..0",
            "4294967296.0.0", "2.9999999999999999999999.0", "2.0.0/evil",
            "٢.٢.٠", "dev", "2.2.0?query=1"
        };
        for (const QString &input : rejected)
            QTest::newRow(qPrintable(QStringLiteral("reject-%1").arg(input)))
                << input << QString();
    }

    void versionParsing() {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const auto result = ReleaseUpdates::parseVersion(input);
        QCOMPARE(result.has_value(), !expected.isEmpty());
        if (result)
            QCOMPARE(result->text(), expected);
    }

    void numericComparison() {
        const auto older = ReleaseUpdates::parseVersion(QStringLiteral("2.9.9"));
        const auto newer = ReleaseUpdates::parseVersion(QStringLiteral("v2.10.0"));
        const auto major = ReleaseUpdates::parseVersion(QStringLiteral("3.0.0"));
        QVERIFY(older && newer && major);
        QVERIFY(*older < *newer);
        QVERIFY(*newer < *major);
        QVERIFY(!(*newer < *newer));
        QVERIFY(!(*major < *newer));
    }

    void validatedReleaseIgnoresUntrustedFields() {
        auto object = releaseObject();
        object.insert(QStringLiteral("html_url"), QStringLiteral("file:///etc/passwd"));
        object.insert(QStringLiteral("body"), QStringLiteral("<img src='https://evil.test/'>"));
        object.insert(QStringLiteral("assets"), QJsonArray{QStringLiteral("https://evil.test/")});
        const auto result = ReleaseUpdates::parseRelease(QJsonDocument(object).toJson());
        QVERIFY(result);
        QCOMPARE(result->version.text(), QStringLiteral("2.3.0"));
        QCOMPARE(result->url(), QUrl(QStringLiteral(
            "https://github.com/DXVSI/Tryx-Linux-GUI/releases/tag/v2.3.0")));
    }

    void malformedRelease_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("syntax") << QByteArray("{broken}");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("oversized") << QByteArray(ReleaseUpdates::MaximumBodyBytes + 1, ' ');
        for (const QString &field : {QStringLiteral("tag_name"), QStringLiteral("draft"),
                                    QStringLiteral("prerelease"), QStringLiteral("published_at")}) {
            auto object = releaseObject();
            object.remove(field);
            QTest::newRow(qPrintable(field + "-missing")) << QJsonDocument(object).toJson();
            object.insert(field, 42);
            QTest::newRow(qPrintable(field + "-wrong-type")) << QJsonDocument(object).toJson();
            object.insert(field, QJsonValue::Null);
            QTest::newRow(qPrintable(field + "-null")) << QJsonDocument(object).toJson();
        }
        for (const QString &field : {QStringLiteral("draft"), QStringLiteral("prerelease")}) {
            auto object = releaseObject();
            object.insert(field, true);
            QTest::newRow(qPrintable(field + "-true")) << QJsonDocument(object).toJson();
        }
        auto object = releaseObject();
        object.insert(QStringLiteral("published_at"), QStringLiteral(" \n"));
        QTest::newRow("unpublished") << QJsonDocument(object).toJson();
        QTest::newRow("prerelease-tag") << releaseBody(QStringLiteral("v2.3.0-beta1"));
        QTest::newRow("url-tag") << releaseBody(QStringLiteral("../../evil"));
    }

    void malformedRelease() {
        QFETCH(QByteArray, body);
        QVERIFY(!ReleaseUpdates::parseRelease(body));
    }

    void startupScheduleAndRequestBoundary() {
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        QCOMPARE(network.requests.size(), 0);
        controller.start();
        QCOMPARE(network.requests.size(), 0); // Startup is queued, never blocking.
        QTRY_COMPARE(network.requests.size(), 1);
        const auto request = network.requests.first();
        QCOMPARE(network.operations.first(), QNetworkAccessManager::GetOperation);
        QCOMPARE(request.url().toString(), QStringLiteral(
            "https://api.github.com/repos/DXVSI/Tryx-Linux-GUI/releases/latest"));
        QCOMPARE(request.rawHeader("Accept"), QByteArray("application/vnd.github+json"));
        QCOMPARE(request.rawHeader("User-Agent"), QByteArray("Tryx-Linux-GUI"));
        QCOMPARE(request.rawHeader("X-GitHub-Api-Version"), QByteArray("2026-03-10"));
        QVERIFY(!request.hasRawHeader("Authorization"));
        QVERIFY(!request.hasRawHeader("Cookie"));
        QCOMPARE(request.transferTimeout(), 10000);
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        for (const auto attribute : {QNetworkRequest::CookieLoadControlAttribute,
                                    QNetworkRequest::CookieSaveControlAttribute,
                                    QNetworkRequest::AuthenticationReuseAttribute})
            QCOMPARE(request.attribute(attribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(),
                 int(QNetworkRequest::AlwaysNetwork));
        QVERIFY(!request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool());
        controller.start();
        now_ = 3600000;
        controller.poll();
        QCOMPARE(network.requests.size(), 1); // Still in flight, even after a delayed event loop.
        network.latest->complete();
        QVERIFY(!controller.updateAvailable()); // Absolute deadline expired.
        controller.poll();
        QCOMPARE(network.requests.size(), 2);
        network.latest->complete();
        now_ += 3599999;
        controller.poll();
        QCOMPARE(network.requests.size(), 2);
        ++now_;
        controller.poll();
        QCOMPARE(network.requests.size(), 3);
        network.latest->complete();
        now_ += 5 * 3600000;
        controller.poll();
        QCOMPARE(network.requests.size(), 4); // No catch-up burst.
        network.latest->complete();
        controller.poll();
        QCOMPARE(network.requests.size(), 4);
    }

    void availabilityEtagAndNotificationSuppression() {
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        QSignalSpy announced(&controller, &ReleaseUpdateController::newReleaseAvailable);
        launch(controller);
        QVERIFY(network.latest);
        network.latest->header("ETag", "\"first\"");
        network.latest->send(releaseBody());
        QVERIFY(!controller.updateAvailable()); // Wait for completed/validated response.
        network.latest->complete();
        QVERIFY(controller.updateAvailable());
        QCOMPARE(controller.availableVersion(), QStringLiteral("2.3.0"));
        QCOMPARE(announced.size(), 1);
        hour(controller);
        QCOMPARE(network.requests.last().rawHeader("If-None-Match"), QByteArray("\"first\""));
        network.latest->status(304);
        network.latest->complete();
        QCOMPARE(announced.size(), 1);
        QCOMPARE(controller.availableVersion(), QStringLiteral("2.3.0"));
        hour(controller);
        network.latest->send(releaseBody("v2.10.0"));
        network.latest->complete();
        QCOMPARE(announced.size(), 2);
        QCOMPARE(controller.availableVersion(), QStringLiteral("2.10.0"));
        hour(controller);
        QVERIFY(!network.requests.last().hasRawHeader("If-None-Match"));
        network.latest->send(releaseBody("v2.3.0"));
        network.latest->complete();
        QCOMPARE(announced.size(), 2); // Never reannounce an older version.
        hour(controller);
        network.latest->send(releaseBody("2.2.0"));
        network.latest->complete();
        QVERIFY(!controller.updateAvailable());
        QVERIFY(controller.availableVersion().isEmpty());
        QVERIFY(controller.releaseUrl().isEmpty());
        hour(controller);
        network.latest->send(releaseBody("2.11.0"));
        network.latest->complete();
        QCOMPARE(announced.size(), 3);
    }

    void silentFailurePreservesValidatedSnapshot_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("error");
        QTest::addColumn<QByteArray>("body");
        for (int status : {301, 302, 307, 308, 403, 404, 429, 500, 503})
            QTest::newRow(qPrintable(QString::number(status))) << status << 0 << releaseBody("9.0.0");
        for (int error : {int(QNetworkReply::HostNotFoundError), int(QNetworkReply::TimeoutError),
                          int(QNetworkReply::SslHandshakeFailedError),
                          int(QNetworkReply::RemoteHostClosedError)})
            QTest::newRow(qPrintable(QStringLiteral("network-%1").arg(error)))
                << 200 << error << releaseBody("9.0.0");
        QTest::newRow("malformed") << 200 << 0 << QByteArray("{}");
        QTest::newRow("oversized") << 200 << 0 << QByteArray(1048577, ' ');
    }

    void silentFailurePreservesValidatedSnapshot() {
        QFETCH(int, status);
        QFETCH(int, error);
        QFETCH(QByteArray, body);
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        QSignalSpy announced(&controller, &ReleaseUpdateController::newReleaseAvailable);
        launch(controller);
        QVERIFY(network.latest);
        network.latest->header("ETag", "\"valid\"");
        network.latest->send(releaseBody());
        network.latest->complete();
        hour(controller);
        network.latest->status(status);
        network.latest->header("ETag", "\"unvalidated\"");
        network.latest->header("Location", "https://evil.test/");
        network.latest->send(body);
        if (!network.latest->isFinished())
            network.latest->complete(QNetworkReply::NetworkError(error));
        QCOMPARE(controller.availableVersion(), QStringLiteral("2.3.0"));
        QCOMPARE(announced.size(), 1);
        QCOMPARE(network.requests.size(), 2); // No redirects or immediate retries.
        hour(controller);
        QCOMPARE(network.requests.last().rawHeader("If-None-Match"), QByteArray("\"valid\""));
    }

    void unknownLocalOlderRemoteAndUnbound304() {
        for (const QString &local : {QStringLiteral("2.2.0"), QStringLiteral("dev"),
                                     QStringLiteral("2.2.0-dev")}) {
            FakeNetwork network;
            ReleaseUpdateController controller(local, &network,
                [this] { return now_; }, [this] { return utc_; });
            QSignalSpy announced(&controller, &ReleaseUpdateController::newReleaseAvailable);
            launch(controller);
            QVERIFY(network.latest);
            network.latest->status(304);
            network.latest->header("ETag", "\"unbound\"");
            network.latest->complete();
            QVERIFY(!controller.updateAvailable());
            hour(controller);
            QVERIFY(!network.requests.last().hasRawHeader("If-None-Match"));
            network.latest->send(releaseBody(local == "2.2.0" ? "2.1.9" : "99.0.0"));
            network.latest->complete();
            QVERIFY(!controller.updateAvailable());
            QCOMPARE(announced.size(), 0);
        }
    }

    void absoluteTimeoutAndBoundedStreaming() {
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        launch(controller);
        QVERIFY(network.latest);
        network.latest->send(releaseBody());
        now_ = 10000;
        network.latest->complete(); // Late success before the timer callback is rejected.
        QVERIFY(!controller.updateAvailable());
        hour(controller);
        // The declared compressed length is irrelevant to the delivered body cap.
        network.latest->header("Content-Length", "1");
        network.latest->send(QByteArray(1048576, ' '));
        QVERIFY(!network.latest->aborted);
        QCOMPARE(controller.body_.size(), 1048576);
        network.latest->send("x");
        QVERIFY(network.latest->aborted);
        QVERIFY(controller.body_.isEmpty());
        QVERIFY(network.latest->largestRead <= 1048577);
        hour(controller);
        const auto body = releaseBody();
        network.latest->send(body + QByteArray(1048576 - body.size(), ' '));
        network.latest->complete();
        QVERIFY(controller.updateAvailable()); // Exact cap is accepted.
        hour(controller);
        controller.deadlineTimer_.setInterval(1); // Exercise real timer-to-abort wiring.
        QTRY_VERIFY(!network.latest || network.latest->aborted);
        QVERIFY(!controller.reply_);
        QVERIFY(controller.updateAvailable());
    }

    void stopAndDestructionCancelWithoutPublishing() {
        FakeNetwork network;
        auto controller = std::unique_ptr<ReleaseUpdateController>(new ReleaseUpdateController(
            "2.2.0", &network, [this] { return now_; }, [this] { return utc_; }));
        launch(*controller);
        QVERIFY(network.latest);
        network.latest->send(releaseBody());
        controller->stop();
        QVERIFY(network.latest->aborted);
        QVERIFY(!controller->updateAvailable());
        QVERIFY(!controller->pollTimer_.isActive());
        QVERIFY(!controller->deadlineTimer_.isActive());
        hour(*controller);
        QCOMPARE(network.requests.size(), 1);
        launch(*controller);
        QVERIFY(!network.latest->aborted);
        controller.reset();
        QVERIFY(network.latest->aborted);
    }

    void rateLimitBackoff_data() {
        QTest::addColumn<QByteArray>("retryAfter");
        QTest::addColumn<QByteArray>("reset");
        QTest::addColumn<qint64>("delay");
        QTest::newRow("seconds") << QByteArray("7200") << QByteArray() << qint64(7200000);
        QTest::newRow("http-date") << QByteArray("Sun, 06 Sep 2026 02:00:00 GMT")
            << QByteArray() << qint64(7200000);
        QTest::newRow("reset") << QByteArray() << QByteArray("1788660000") << qint64(7200000);
        QTest::newRow("latest") << QByteArray("7200") << QByteArray("1788663600") << qint64(10800000);
        for (const QByteArray &invalid : {QByteArray("-1"), QByteArray("nan"),
                                        QByteArray("99999999999999999999"), QByteArray("1.5")})
            QTest::newRow(invalid.constData()) << invalid << invalid << qint64(3600000);
    }

    void rateLimitBackoff() {
        QFETCH(QByteArray, retryAfter);
        QFETCH(QByteArray, reset);
        QFETCH(qint64, delay);
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        launch(controller);
        QVERIFY(network.latest);
        network.latest->status(429);
        network.latest->header("Retry-After", retryAfter);
        network.latest->header("X-RateLimit-Remaining", "0");
        network.latest->header("X-RateLimit-Reset", reset);
        network.latest->complete(QNetworkReply::ContentAccessDenied);
        now_ = delay - 1;
        utc_ -= 86400000; // Wall-clock changes must not shorten monotonic cooldown.
        controller.poll();
        QCOMPARE(network.requests.size(), 1);
        ++now_;
        controller.poll();
        QCOMPARE(network.requests.size(), 2);
    }

    void notificationFailureKeepsSettingsWithoutShowingWindow() {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        const QString service = QStringLiteral("org.freedesktop.Notifications");
        const QString path = QStringLiteral("/org/freedesktop/Notifications");
        NotificationService notifications;
        QVERIFY(bus.registerService(service));
        QVERIFY(bus.registerObject(path, &notifications, QDBusConnection::ExportAllSlots));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterObject(path);
            bus.unregisterService(service);
        });
        LinuxTrayController tray;
        FakeNetwork network;
        ReleaseUpdateController controller("2.2.0", &network,
            [this] { return now_; }, [this] { return utc_; });
        QSignalSpy failure(&tray, &LinuxTrayController::notificationFailed);
        QSignalSpy showWindow(&tray, &LinuxTrayController::showRequested);
        connect(&controller, &ReleaseUpdateController::newReleaseAvailable,
                &tray, [&tray](const QString &version) {
            tray.showNotification(QStringLiteral("Application update"), version);
        });
        launch(controller);
        network.latest->send(releaseBody());
        network.latest->complete();
        QTRY_COMPARE(notifications.calls, 1);
        QCOMPARE(notifications.lastBody, QStringLiteral("2.3.0"));
        QVERIFY(controller.updateAvailable());
        QCOMPARE(failure.size(), 0);
        notifications.reject = true;
        hour(controller);
        network.latest->send(releaseBody("v2.4.0"));
        network.latest->complete();
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(notifications.calls, 2);
        QCOMPARE(controller.availableVersion(), QStringLiteral("2.4.0"));
        QCOMPARE(showWindow.size(), 0);
        hour(controller);
        network.latest->send(releaseBody("v2.4.0"));
        network.latest->complete();
        QCOMPARE(notifications.calls, 2); // Failure does not cause popup retries.
    }

    void slowNotificationServiceDoesNotBlockGuiThread() {
        if (!QCoreApplication::arguments().contains(
                QStringLiteral("--internal-fresh-notifications-test"))) {
            // Qt caches interface introspection per process. A prior responsive
            // notification test must not hide a regression in the first call.
            QProcess client;
            client.start(QCoreApplication::applicationFilePath(),
                         {QStringLiteral("--internal-fresh-notifications-test")});
            QVERIFY(client.waitForFinished(5000));
            QVERIFY2(client.exitStatus() == QProcess::NormalExit && client.exitCode() == 0,
                     client.readAll().constData());
            return;
        }
        QProcess provider;
        provider.start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--internal-slow-notifications")});
        const auto cleanup = qScopeGuard([&] {
            provider.terminate();
            if (!provider.waitForFinished(2000)) {
                provider.kill();
                provider.waitForFinished(2000);
            }
        });
        QVERIFY(provider.waitForReadyRead(3000));
        QCOMPARE(provider.readAllStandardOutput().trimmed(), QByteArray("ready"));
        LinuxTrayController tray;
        QSignalSpy failed(&tray, &LinuxTrayController::notificationFailed);
        QElapsedTimer elapsed;
        elapsed.start();
        tray.showNotification(QStringLiteral("Update"), QStringLiteral("2.3.0"));
        QVERIFY2(elapsed.elapsed() < 500, "Notification dispatch blocked on D-Bus introspection");
        bool heartbeat = false;
        QTimer::singleShot(0, &tray, [&] { heartbeat = true; });
        QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 500);
        QTRY_VERIFY_WITH_TIMEOUT(tray.findChildren<QDBusPendingCallWatcher *>().isEmpty(), 2500);
        QCOMPARE(failed.size(), 0);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--internal-slow-notifications"))) {
        if (qEnvironmentVariable("TRYX_RELEASE_UPDATE_TEST_ISOLATED") != "1")
            return 2;
        auto bus = QDBusConnection::sessionBus();
        SlowNotificationService service;
        if (!bus.registerVirtualObject(QStringLiteral("/org/freedesktop/Notifications"), &service) ||
            !bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
            return 2;
        std::puts("ready");
        std::fflush(stdout);
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    ReleaseUpdateControllerTests tests;
    if (app.arguments().contains(QStringLiteral("--internal-fresh-notifications-test")))
        return QTest::qExec(&tests, {app.applicationFilePath(),
            QStringLiteral("slowNotificationServiceDoesNotBlockGuiThread")});
    return QTest::qExec(&tests, argc, argv);
}
#include "releaseupdatecontroller_tests.moc"
