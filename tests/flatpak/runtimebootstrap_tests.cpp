#include "runtimebootstrap.h"
#include "runtimecontract.h"
#include "flatpakruntimeownership.h"
#include "packagingcontext.h"
#include "guiautostart.h"

#include <QtTest>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFile>
#include <QLocalServer>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <signal.h>
#include <cstdlib>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

class FakeRuntime final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")
public slots:
    uint GetRuntimeApiVersion() { return tryxRuntimeApiVersion(); }
    void Quit() { QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit); }
};

namespace {
QString owner(const QString &service = tryxRuntimeServiceName()) {
    const QDBusReply<QString> reply =
        QDBusConnection::sessionBus().interface()->serviceOwner(service);
    return reply.isValid() ? reply.value() : QString();
}

quickbootstrap::RuntimeBootstrapOptions options(const QString &pidFile, int delay = 0) {
    quickbootstrap::RuntimeBootstrapOptions value;
    // A real executable that rejects systemctl arguments. A missing program
    // would let the native bootstrap silently take its fallback path.
    value.systemctlProgram = QCoreApplication::applicationFilePath();
    value.discoverDevelopmentRuntime = false;
    value.installedRuntimeFallbackProgram = QCoreApplication::applicationFilePath();
    value.installedRuntimeFallbackArguments =
        {QStringLiteral("--fake-runtime"), pidFile, QString::number(delay)};
    value.startupTimeoutMs = 600;
    value.dbusCallTimeoutMs = 100;
    return value;
}

qint64 recordedPid(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0;
}
}

class FlatpakBootstrapTests final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        if (!owner().isEmpty()) {
            QDBusInterface runtime(owner(), tryxRuntimeObjectPath(),
                                   tryxRuntimeOperationsInterfaceName(),
                                   QDBusConnection::sessionBus());
            runtime.call(QStringLiteral("Quit"));
            QTRY_VERIFY_WITH_TIMEOUT(owner().isEmpty(), 2000);
        }
    }

    void startsBundledRuntimeWithoutSystemd() {
        QTemporaryDir directory;
        const QString pidFile = directory.filePath("runtime.pid");
        QString error;
        QVERIFY2(quickbootstrap::ensureRuntimeService(&error, options(pidFile)), qPrintable(error));
        QVERIFY(recordedPid(pidFile) > 0);
        QVERIFY(!owner().isEmpty());
        QVERIFY(owner(tryx::packaging::nativeRuntimeService()) != owner());
    }

    void acceptsExistingSandboxOwnerWithoutStartingAnotherChild() {
        QTemporaryDir directory;
        QString error;
        QVERIFY2(quickbootstrap::ensureRuntimeService(
                     &error, options(directory.filePath("first.pid"))), qPrintable(error));
        const QString firstOwner = owner();
        auto repeated = options(directory.filePath("second.pid"));
        repeated.installedRuntimeFallbackProgram = QStringLiteral("/nonexistent-bundled-runtime");
        QVERIFY2(quickbootstrap::ensureRuntimeService(&error, repeated), qPrintable(error));
        QCOMPARE(owner(), firstOwner);
        QVERIFY(!QFileInfo::exists(directory.filePath("second.pid")));
    }

    void nativeRuntimeBlocksStartAndIsNotStopped() {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(tryx::packaging::nativeRuntimeService()));
        const auto release = qScopeGuard([&]() {
            bus.unregisterService(tryx::packaging::nativeRuntimeService());
        });
        QTemporaryDir directory;
        QString error;
        QVERIFY(!quickbootstrap::ensureRuntimeService(
            &error, options(directory.filePath("unexpected.pid"))));
        QVERIFY(!error.isEmpty());
        QCOMPARE(owner(tryx::packaging::nativeRuntimeService()), bus.baseService());
        QVERIFY(!QFileInfo::exists(directory.filePath("unexpected.pid")));
    }

    void apiTimeoutTerminatesOnlyTheChildWeStarted() {
        QTemporaryDir directory;
        const QString pidFile = directory.filePath("slow.pid");
        QString error;
        QVERIFY(!quickbootstrap::ensureRuntimeService(&error, options(pidFile, 10000)));
        QVERIFY2(!error.isEmpty(), "A startup timeout must be explicit");
        const qint64 pid = recordedPid(pidFile);
        QVERIFY(pid > 0);
        QTRY_VERIFY_WITH_TIMEOUT(owner().isEmpty(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(owner(tryx::packaging::nativeRuntimeService()).isEmpty(), 2000);
    }

    void runtimePathsAreSharedAndApplicationScoped() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath("app")));
        const QByteArray previous = qgetenv("XDG_RUNTIME_DIR");
        const auto restore = qScopeGuard([&]() { qputenv("XDG_RUNTIME_DIR", previous); });
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(directory.path()));
        const QString prefix = tryx::packaging::runtimeDirectory();
        QVERIFY(prefix.endsWith(QStringLiteral("/app/") + tryx::packaging::appId()));
        QVERIFY(tryxRuntimeMediaInboxPath().startsWith(prefix + QLatin1Char('/')));
        QVERIFY(tryxRuntimeMediaSpoolPath().startsWith(prefix + QLatin1Char('/')));
        QVERIFY(tryxRuntimeDeviceMediaOutboxPath().startsWith(prefix + QLatin1Char('/')));
        QCOMPARE(tryxRuntimeServiceName(), tryx::packaging::flatpakRuntimeService());
        QCOMPARE(tryxRuntimeObjectPath(), QStringLiteral("/org/tryx/Panorama"));
    }

    void singleInstanceAcceptsSandboxAppParentAlias() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/tryx-rt-XXXXXX"));
        QVERIFY(directory.isValid());
        const QString runtime = directory.filePath("runtime");
        const QString apps = directory.filePath("real/app");
        const QString application = QDir(apps).filePath(tryx::packaging::appId());
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QDir().mkpath(application));
        QVERIFY(QFile::setPermissions(application, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::link(apps, QDir(runtime).filePath("app")));
        const QByteArray previous = qgetenv("XDG_RUNTIME_DIR");
        const auto restore = qScopeGuard([&]() { qputenv("XDG_RUNTIME_DIR", previous); });
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime));

        const QString socket = quickbootstrap::instanceSocketPath();
        QVERIFY2(!socket.isEmpty(), "The sandbox-owned app parent alias must allow ordinary startup");
        QCOMPARE(tryx::packaging::runtimeDirectory(), application);
        quickbootstrap::SingleInstanceGuard guard(socket);
        QLocalServer server;
        QString error;
        const auto result = guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error);
        QVERIFY2(result == quickbootstrap::SingleInstanceAcquireResult::Primary, qPrintable(error));
        QVERIFY(server.isListening());
        QVERIFY(tryxRuntimeMediaInboxPath().startsWith(application + QLatin1Char('/')));
        QVERIFY(tryxRuntimeMediaSpoolPath().startsWith(application + QLatin1Char('/')));
        QVERIFY(tryxRuntimeDeviceMediaOutboxPath().startsWith(application + QLatin1Char('/')));
    }

    void runtimeAliasDoesNotHideUnsafeApplicationDirectory_data() {
        QTest::addColumn<bool>("symlink");
        QTest::newRow("symlink-leaf") << true;
        QTest::newRow("non-private-leaf") << false;
    }

    void runtimeAliasDoesNotHideUnsafeApplicationDirectory() {
        QFETCH(bool, symlink);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString runtime = directory.filePath("runtime");
        const QString apps = directory.filePath("real/app");
        const QString application = QDir(apps).filePath(tryx::packaging::appId());
        QVERIFY(QDir().mkpath(runtime));
        QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QDir().mkpath(apps));
        if (symlink) {
            const QString target = directory.filePath("unexpected-target");
            QVERIFY(QDir().mkpath(target));
            QVERIFY(QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            QVERIFY(QFile::link(target, application));
        } else {
            QVERIFY(QDir().mkpath(application));
            QVERIFY(QFile::setPermissions(application, QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner | QFile::ReadGroup));
        }
        QVERIFY(QFile::link(apps, QDir(runtime).filePath("app")));
        const QByteArray previous = qgetenv("XDG_RUNTIME_DIR");
        const auto restore = qScopeGuard([&]() { qputenv("XDG_RUNTIME_DIR", previous); });
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime));

        QCOMPARE(tryx::packaging::runtimeDirectory(), application);
        QVERIFY(quickbootstrap::instanceSocketPath().isEmpty());
        quickbootstrap::SingleInstanceGuard guard(QDir(application).filePath("tryx-panorama-manager.instance"));
        QLocalServer server;
        QString error;
        QCOMPARE(guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error),
                 quickbootstrap::SingleInstanceAcquireResult::Failed);
        QVERIFY(!error.isEmpty());
        QVERIFY(!server.isListening());
    }

    void missingSandboxAppParentFailsClosed() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray previous = qgetenv("XDG_RUNTIME_DIR");
        const auto restore = qScopeGuard([&]() { qputenv("XDG_RUNTIME_DIR", previous); });
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(directory.path()));
        QVERIFY(tryx::packaging::runtimeDirectory().isEmpty());
        QVERIFY(quickbootstrap::instanceSocketPath().isEmpty());
        QVERIFY(tryxRuntimeMediaInboxPath().isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("app")));
    }

    void crashedInstanceRecoversDespiteReusedNamespacePid() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/tryx-lock-XXXXXX"));
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath("instance");
        QProcess crashed;
        crashed.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--crash-instance"), socket});
        QVERIFY(crashed.waitForStarted(2000));
        QVERIFY(crashed.waitForFinished(3000));
        QCOMPARE(crashed.exitCode(), 0);
        QVERIFY(QFileInfo::exists(socket));

        // A fresh Flatpak PID namespace reuses the old GUI PID and executable
        // name. Model that ambiguity with a legacy QLockFile claiming this live
        // test process. It must neither block the kernel lease nor be deleted.
        QLockFile reusedPid(socket + QStringLiteral(".lock"));
        reusedPid.setStaleLockTime(0);
        QVERIFY(reusedPid.tryLock(0));
        quickbootstrap::SingleInstanceGuard guard(socket);
        QLocalServer server;
        QString error;
        const auto result = guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error);
        QVERIFY2(result == quickbootstrap::SingleInstanceAcquireResult::Primary, qPrintable(error));
        QVERIFY(server.isListening());
        QVERIFY(reusedPid.isLocked());
        QVERIFY(QFileInfo::exists(socket + QStringLiteral(".lock")));
    }

    void kernelLeaseWaitsForAStartingOwner() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath("instance");
        QFile lease(socket + QStringLiteral(".flock"));
        QVERIFY(lease.open(QIODevice::ReadWrite));
        QVERIFY(lease.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(::flock(lease.handle(), LOCK_EX | LOCK_NB), 0);
        quickbootstrap::SingleInstanceGuard guard(socket);
        QLocalServer server;
        QString error;
        QCOMPARE(guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error),
                 quickbootstrap::SingleInstanceAcquireResult::Failed);
        QVERIFY(error.contains(QStringLiteral("did not make its socket ready")));
        QVERIFY(!QFileInfo::exists(socket));
        QCOMPARE(::flock(lease.handle(), LOCK_UN), 0);
        QCOMPARE(guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error),
                 quickbootstrap::SingleInstanceAcquireResult::Primary);
    }

    void replacedLeaseCannotRemoveAStaleSocket() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/tryx-race-XXXXXX"));
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath("instance");
        QProcess crashed;
        crashed.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--crash-instance"), socket});
        QVERIFY(crashed.waitForStarted(2000));
        QVERIFY(crashed.waitForFinished(3000));
        QCOMPARE(crashed.exitCode(), 0);
        struct stat before {};
        QCOMPARE(::lstat(QFile::encodeName(socket).constData(), &before), 0);
        QVERIFY(S_ISSOCK(before.st_mode));
        quickbootstrap::testing::setAfterLeaseAcquiredBeforeSocketCleanupHook(
            [](const QString &path) {
                const QString lease = path + QStringLiteral(".flock");
                if (!QFile::rename(lease, lease + QStringLiteral(".displaced")))
                    return;
                QFile replacement(lease);
                if (replacement.open(QIODevice::ReadWrite)) {
                    replacement.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                    replacement.write("replacement");
                }
            });
        const auto restore = qScopeGuard([]() {
            quickbootstrap::testing::clearAfterLeaseAcquiredBeforeSocketCleanupHook();
        });
        quickbootstrap::SingleInstanceGuard guard(socket);
        QLocalServer server;
        QString error;
        QCOMPARE(guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error),
                 quickbootstrap::SingleInstanceAcquireResult::Failed);
        struct stat after {};
        QCOMPARE(::lstat(QFile::encodeName(socket).constData(), &after), 0);
        QCOMPARE(after.st_dev, before.st_dev);
        QCOMPARE(after.st_ino, before.st_ino);
        QFile replacement(socket + QStringLiteral(".flock"));
        QVERIFY(replacement.open(QIODevice::ReadOnly));
        QCOMPARE(replacement.readAll(), QByteArray("replacement"));
    }

    void unsafeKernelLeaseIsRejected_data() {
        QTest::addColumn<QString>("kind");
        for (const QString &kind : {QStringLiteral("symlink"), QStringLiteral("hardlink"),
                                   QStringLiteral("fifo"), QStringLiteral("permissions"),
                                   QStringLiteral("replacement")})
            QTest::newRow(qPrintable(kind)) << kind;
    }

    void unsafeKernelLeaseIsRejected() {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath("instance");
        const QString lease = socket + QStringLiteral(".flock");
        QFile target(directory.filePath("untouched"));
        QVERIFY(target.open(QIODevice::ReadWrite));
        QVERIFY(target.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(target.write("preserved"), 9);
        target.close();
        if (kind == QStringLiteral("symlink")) {
            QVERIFY(QFile::link(target.fileName(), lease));
        } else if (kind == QStringLiteral("hardlink")) {
            QCOMPARE(::link(QFile::encodeName(target.fileName()).constData(),
                            QFile::encodeName(lease).constData()), 0);
        } else if (kind == QStringLiteral("fifo")) {
            QCOMPARE(::mkfifo(QFile::encodeName(lease).constData(), 0600), 0);
        } else if (kind == QStringLiteral("permissions")) {
            QVERIFY(QFile::copy(target.fileName(), lease));
            QVERIFY(QFile::setPermissions(lease, QFile::ReadOwner | QFile::WriteOwner | QFile::WriteGroup));
        }
        quickbootstrap::SingleInstanceGuard guard(socket);
        if (kind == QStringLiteral("replacement")) {
            QVERIFY(QFile::rename(lease, lease + QStringLiteral(".displaced")));
            QVERIFY(QFile::copy(target.fileName(), lease));
        }
        QLocalServer server;
        QString error;
        QCOMPARE(guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error),
                 quickbootstrap::SingleInstanceAcquireResult::Failed);
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(socket));
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), QByteArray("preserved"));
    }

    void autostartIsExplicitlyUnavailableWithoutWritingPrivateDesktopFiles() {
        QTemporaryDir directory;
        gui_autostart::testing::setUserConfigDirectoryOverride(directory.path());
        const auto restore = qScopeGuard([]() {
            gui_autostart::testing::clearUserConfigDirectoryOverride();
        });
        const auto state = gui_autostart::query();
        QVERIFY(!state.available);
        QVERIFY(!state.enabled);
        QVERIFY(!state.error.isEmpty());
        QString error;
        QVERIFY(!gui_autostart::setEnabled(true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::Hidden |
                                                QDir::NoDotAndDotDot).isEmpty());
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariable("TRYX_FLATPAK_TEST_ISOLATED") != QStringLiteral("1"))
        return 2;
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("--user"))
        return 5;
    if (app.arguments().value(1) == QStringLiteral("--crash-instance")) {
        quickbootstrap::SingleInstanceGuard guard(app.arguments().value(2));
        QLocalServer server;
        QString error;
        if (guard.acquire(&server, quickbootstrap::InstanceLaunchIntent::Manual, &error) !=
            quickbootstrap::SingleInstanceAcquireResult::Primary)
            return 6;
        std::_Exit(0);
    }
    if (app.arguments().value(1) == QStringLiteral("--fake-runtime")) {
        QFile pidFile(app.arguments().value(2));
        if (!pidFile.open(QIODevice::WriteOnly))
            return 3;
        pidFile.write(QByteArray::number(::getpid()));
        pidFile.close();
        FlatpakRuntimeOwnership ownership(QDBusConnection::sessionBus());
        QString error;
        if (!ownership.acquire(&error))
            return 4;
        QObject::connect(&ownership, &FlatpakRuntimeOwnership::ownershipLost,
                         &app, &QCoreApplication::quit);
        FakeRuntime runtime;
        QTimer::singleShot(app.arguments().value(3).toInt(), &app, [&runtime]() {
            QDBusConnection::sessionBus().registerObject(
                tryxRuntimeObjectPath(), &runtime, QDBusConnection::ExportAllSlots);
        });
        return app.exec();
    }
    FlatpakBootstrapTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "runtimebootstrap_tests.moc"
