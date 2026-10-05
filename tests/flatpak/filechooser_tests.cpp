#include "portalfilechooser.h"
#include <QtTest>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusMetaType>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString path = QStringLiteral("/org/freedesktop/portal/desktop");
}

class ChooserRequest final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    bool closed = false;
public slots:
    void Close() { closed = true; }
signals:
    void Response(uint result, const QVariantMap &results);
};

class FakeChooser final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.FileChooser")
    Q_PROPERTY(uint version READ version)
public:
    uint version() const { return apiVersion; }
    uint apiVersion = 4;
    uint result = 0;
    bool automatic = true;
    bool wrongPath = false;
    QStringList uris{QStringLiteral("file:///run/user/1000/doc/synthetic/image.jpg")};
    QVariantMap received;
    int calls = 0;
    ChooserRequest request;
    QString requestPath;
    const QString connectionName = QStringLiteral("fake-chooser-") +
                                   QUuid::createUuid().toString(QUuid::Id128);
    QDBusConnection bus = QDBusConnection::connectToBus(
        QDBusConnection::SessionBus, connectionName);
    bool install() {
        return bus.registerService(service) && bus.registerObject(path, this,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
    }
    ~FakeChooser() {
        bus.unregisterObject(path);
        if (!requestPath.isEmpty()) bus.unregisterObject(requestPath);
        bus.unregisterService(service);
        QDBusConnection::disconnectFromBus(connectionName);
    }
public slots:
    QDBusObjectPath OpenFile(const QString &, const QString &, const QVariantMap &options) {
        ++calls;
        received = options;
        QString sender = message().service().mid(1);
        sender.replace('.', '_');
        requestPath = path + "/request/" + sender + "/" + options.value("handle_token").toString();
        bus.registerObject(requestPath, &request, QDBusConnection::ExportAllSlots |
                                                QDBusConnection::ExportAllSignals);
        // Intentionally before the method reply: supported portal ordering.
        if (automatic)
            emit request.Response(result, {{QStringLiteral("uris"), uris}});
        return QDBusObjectPath(wrongPath ? requestPath + "_wrong" : requestPath);
    }
};

class FileChooserTests final : public QObject {
    Q_OBJECT
private slots:
    void acceptsDocumentFileAfterEarlyResponse() {
        FakeChooser portal;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        connect(&chooser, &PortalFileChooser::failed, this,
                [](const QString &error) { qWarning().noquote() << error; });
        chooser.open("Import", QUrl::fromLocalFile("/tmp/synthetic-folder"));
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 1500);
        QCOMPARE(selected.first().first().toUrl().toString(), portal.uris.first());
        QVERIFY(!chooser.busy());
        QCOMPARE(portal.received.value("multiple").toBool(), false);
        QCOMPARE(portal.received.value("directory").toBool(), false);
        QCOMPARE(portal.received.value("current_folder").toByteArray(),
                 QByteArray("/tmp/synthetic-folder\0", 22));
    }
    void requestsDirectoryGrant() {
        FakeChooser portal;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::Directory);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Export");
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 1500);
        QCOMPARE(portal.received.value("directory").toBool(), true);
    }
    void resolvesOnlyDocumentsRootAlias_data() {
        QTest::addColumn<QString>("relativePath");
        QTest::addColumn<QString>("expectedPath");
        QTest::newRow("portal-directory") << QStringLiteral("doc/output")
            << QStringLiteral("output");
        QTest::newRow("preserve-descendant-symlink") << QStringLiteral("doc/alias")
            << QStringLiteral("alias");
        QTest::newRow("preserve-ordinary-symlink") << QStringLiteral("ordinary")
            << QString();
        QTest::newRow("do-not-match-prefix-sibling") << QStringLiteral("doc-other/output")
            << QString();
    }
    void resolvesOnlyDocumentsRootAlias() {
        QFETCH(QString, relativePath);
        QFETCH(QString, expectedPath);
        QTemporaryDir runtime;
        QTemporaryDir documents;
        QVERIFY(runtime.isValid());
        QVERIFY(documents.isValid());
        QVERIFY(QDir().mkdir(documents.filePath("output")));
        QVERIFY(QFile::link(documents.path(), runtime.filePath("doc")));
        QVERIFY(QFile::link(documents.filePath("output"), documents.filePath("alias")));
        QVERIFY(QFile::link(documents.filePath("output"), runtime.filePath("ordinary")));
        const bool hadRuntime = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
        const QByteArray previousRuntime = qgetenv("XDG_RUNTIME_DIR");
        const auto restore = qScopeGuard([&]() {
            if (hadRuntime) qputenv("XDG_RUNTIME_DIR", previousRuntime);
            else qunsetenv("XDG_RUNTIME_DIR");
        });
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
        FakeChooser portal;
        portal.uris = {QUrl::fromLocalFile(runtime.filePath(relativePath)).toString()};
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::Directory);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Export");
        QTRY_COMPARE_WITH_TIMEOUT(selected.size(), 1, 1500);
        QCOMPARE(selected.first().first().toUrl(), QUrl::fromLocalFile(
            expectedPath.isEmpty() ? runtime.filePath(relativePath)
                                   : documents.filePath(expectedPath)));
    }
    void cancellationDoesNotPublishAFile() {
        FakeChooser portal;
        portal.result = 1;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile);
        QSignalSpy cancelled(&chooser, &PortalFileChooser::cancelled);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Import");
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.size(), 1, 1500);
        QCOMPARE(selected.size(), 0);
    }
    void rejectsUntrustedResult_data() {
        QTest::addColumn<QStringList>("uris");
        QTest::newRow("remote") << QStringList{"https://example.com/image.jpg"};
        QTest::newRow("nonlocal-host") << QStringList{"file://example.com/tmp/image.jpg"};
        QTest::newRow("relative") << QStringList{"image.jpg"};
        QTest::newRow("empty") << QStringList{};
        QTest::newRow("multiple") << QStringList{"file:///tmp/a", "file:///tmp/b"};
    }
    void rejectsUntrustedResult() {
        QFETCH(QStringList, uris);
        FakeChooser portal;
        portal.uris = uris;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile);
        QSignalSpy failed(&chooser, &PortalFileChooser::failed);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Import");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QCOMPARE(selected.size(), 0);
    }
    void rejectsEarlySuccessForWrongRequestPath() {
        FakeChooser portal;
        portal.wrongPath = true;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile);
        QSignalSpy failed(&chooser, &PortalFileChooser::failed);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Import");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QCOMPARE(selected.size(), 0);
    }
    void closesTimedOutRequestAndIgnoresLateResponse() {
        FakeChooser portal;
        portal.automatic = false;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile, nullptr, 100);
        QSignalSpy failed(&chooser, &PortalFileChooser::failed);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("Import");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QTRY_VERIFY(portal.request.closed);
        emit portal.request.Response(0, {{QStringLiteral("uris"), portal.uris}});
        QCoreApplication::processEvents();
        QCOMPARE(selected.size(), 0);
        QVERIFY(!chooser.busy());
    }
    void ownerLossCancelsAndDoesNotReopen() {
        FakeChooser portal;
        portal.automatic = false;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::Directory);
        QSignalSpy failed(&chooser, &PortalFileChooser::failed);
        chooser.open("Export");
        QTRY_COMPARE(portal.calls, 1);
        QVERIFY(portal.bus.unregisterService(service));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QVERIFY(!chooser.busy());
        QCOMPARE(portal.calls, 1);
    }
    void oldPortalCannotChooseDirectories() {
        FakeChooser portal;
        portal.apiVersion = 2;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::Directory);
        QSignalSpy failed(&chooser, &PortalFileChooser::failed);
        chooser.open("Export");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QCOMPARE(portal.calls, 0);
    }

    void cancelAndReopenIgnoresOldRequestResponse() {
        FakeChooser portal;
        portal.automatic = false;
        QVERIFY(portal.install());
        PortalFileChooser chooser(PortalFileChooser::Mode::MediaFile);
        QSignalSpy selected(&chooser, &PortalFileChooser::selected);
        chooser.open("First");
        QTRY_COMPARE(portal.calls, 1);
        const QString oldPath = portal.requestPath;
        chooser.cancel();
        chooser.open("Second");
        QTRY_COMPARE(portal.calls, 2);
        auto oldResponse = QDBusMessage::createSignal(oldPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
        oldResponse.setArguments({uint(0), QVariantMap{{QStringLiteral("uris"),
            QStringList{QStringLiteral("file:///tmp/old.png")}}}});
        QVERIFY(portal.bus.send(oldResponse));
        QTest::qWait(25);
        QCOMPARE(selected.count(), 0);
        emit portal.request.Response(0, {{QStringLiteral("uris"), portal.uris}});
        QTRY_COMPARE(selected.count(), 1);
        QCOMPARE(selected.first().first().toUrl().toString(), portal.uris.first());
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariable("TRYX_FLATPAK_TEST_ISOLATED") != QStringLiteral("1")) return 2;
    QCoreApplication app(argc, argv);
    FileChooserTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "filechooser_tests.moc"
