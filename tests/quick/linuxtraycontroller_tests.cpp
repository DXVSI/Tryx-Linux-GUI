#include "linuxtraycontroller.h"
#include "startupvisibilitycontroller.h"
#include "windowchromecontroller.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEvent>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <QtTest>

namespace {

const QString kWatcherService =
    QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kWatcherPath =
    QStringLiteral("/StatusNotifierWatcher");
const QString kWatcherInterface =
    QStringLiteral("org.kde.StatusNotifierWatcher");
const QString kNotificationsService =
    QStringLiteral("org.freedesktop.Notifications");
const QString kNotificationsPath =
    QStringLiteral("/org/freedesktop/Notifications");
const QString kQuitProbeArgument =
    QStringLiteral("--internal-tray-quit-probe");
const QString kQuitPolicyCloseProbeArgument =
    QStringLiteral("--internal-quit-policy-close-probe");
const QString kNoHostCloseProbeArgument =
    QStringLiteral("--internal-no-host-close-probe");
const QString kGuardedCloseProbeArgument =
    QStringLiteral("--internal-guarded-close-probe");

class TrayCloseFilter final : public QObject {
public:
    explicit TrayCloseFilter(
        WindowChromeController *windowChrome,
        QObject *parent = nullptr)
        : QObject(parent), windowChrome_(windowChrome) {}

protected:
    bool eventFilter(
        QObject *watched, QEvent *event) override {
        // Mirror Main.qml: reject a normal close only when the Hide policy is
        // active and tray support is available. Explicit tray Quit must still
        // end the event loop.
        if (event->type() == QEvent::Close &&
            windowChrome_->handleCloseRequest()) {
            event->ignore();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    WindowChromeController *windowChrome_ = nullptr;
};

class GuardedLifecycleProbe final : public QObject {
    Q_OBJECT

public:
    enum class LeaveIntent {
        None,
        HideToTray,
        WindowQuit,
        ExplicitQuit,
    };
    Q_ENUM(LeaveIntent)

    GuardedLifecycleProbe(
        QGuiApplication *application,
        WindowChromeController *windowChrome,
        QObject *parent = nullptr)
        : QObject(parent),
          application_(application),
          windowChrome_(windowChrome) {}

    LeaveIntent pendingIntent() const {
        return pendingIntent_;
    }

    int closeEventCount() const {
        return closeEventCount_;
    }

    void requestExplicitQuit() {
        publishIntent(LeaveIntent::ExplicitQuit);
    }

    void approvePendingWithoutApply() {
        const LeaveIntent approved = pendingIntent_;
        pendingIntent_ = LeaveIntent::None;
        switch (approved) {
        case LeaveIntent::HideToTray:
            windowChrome_->hideWindowToTray();
            break;
        case LeaveIntent::WindowQuit:
            approvedCloseBypass_ = true;
            windowChrome_->closeWindow();
            break;
        case LeaveIntent::ExplicitQuit:
            application_->exit(0);
            break;
        case LeaveIntent::None:
            break;
        }
    }

signals:
    void guardedIntentPublished();
    void displayApplyRequested();

protected:
    bool eventFilter(
        QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Close) {
            return QObject::eventFilter(watched, event);
        }
        ++closeEventCount_;
        if (approvedCloseBypass_) {
            approvedCloseBypass_ = false;
            return false;
        }
        event->ignore();
        publishIntent(
            windowChrome_->closeWouldHideToTray()
                ? LeaveIntent::HideToTray
                : LeaveIntent::WindowQuit);
        return true;
    }

private:
    void publishIntent(LeaveIntent intent) {
        if (pendingIntent_ != LeaveIntent::None) {
            return;
        }
        pendingIntent_ = intent;
        emit guardedIntentPublished();
    }

    QGuiApplication *application_ = nullptr;
    WindowChromeController *windowChrome_ = nullptr;
    LeaveIntent pendingIntent_ = LeaveIntent::None;
    bool approvedCloseBypass_ = false;
    int closeEventCount_ = 0;
};

class MockStatusNotifierWatcher final : public QObject {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(bool IsStatusNotifierHostRegistered
                   READ hostRegistered)

public:
    bool hostRegistered() const {
        return hostRegistered_;
    }
    int registrationCount() const {
        return registrationCount_;
    }
    QString registeredItem() const {
        return registeredItem_;
    }

    void setHostRegistered(bool registered) {
        if (hostRegistered_ == registered) {
            return;
        }
        hostRegistered_ = registered;
        if (hostRegistered_) {
            emit StatusNotifierHostRegistered();
        } else {
            emit StatusNotifierHostUnregistered();
        }
    }

public slots:
    void RegisterStatusNotifierItem(
        const QString &serviceOrPath) {
        ++registrationCount_;
        registeredItem_ = serviceOrPath;
    }

signals:
    void StatusNotifierHostRegistered();
    void StatusNotifierHostUnregistered();

private:
    bool hostRegistered_ = true;
    int registrationCount_ = 0;
    QString registeredItem_;
};

class MockNotifications final : public QObject {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "org.freedesktop.Notifications")

public:
    int notifyCount() const {
        return notifyCount_;
    }
    QString lastSummary() const {
        return lastSummary_;
    }
    QString lastBody() const {
        return lastBody_;
    }

public slots:
    uint Notify(
        const QString &appName, uint replacesId,
        const QString &appIcon, const QString &summary,
        const QString &body, const QStringList &actions,
        const QVariantMap &hints, int timeout) {
        Q_UNUSED(appName);
        Q_UNUSED(replacesId);
        Q_UNUSED(appIcon);
        Q_UNUSED(actions);
        Q_UNUSED(hints);
        Q_UNUSED(timeout);
        ++notifyCount_;
        lastSummary_ = summary;
        lastBody_ = body;
        return 41;
    }

private:
    int notifyCount_ = 0;
    QString lastSummary_;
    QString lastBody_;
};

bool registerMockWatcher(
    QDBusConnection bus,
    MockStatusNotifierWatcher *watcher) {
    const auto flags =
        QDBusConnection::ExportAllProperties |
        QDBusConnection::ExportAllSlots |
        QDBusConnection::ExportAllSignals;
    return bus.registerObject(
               kWatcherPath, watcher, flags) &&
           bus.registerService(kWatcherService);
}

void unregisterMockWatcher(QDBusConnection bus) {
    bus.unregisterService(kWatcherService);
    bus.unregisterObject(kWatcherPath);
}

int runTrayQuitProbe(QGuiApplication &application) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return 70;
    }

    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setTrayAvailable(true);
    GuardedLifecycleProbe lifecycle(
        &application, &windowChrome);
    window.installEventFilter(&lifecycle);
    window.show();

    LinuxTrayController tray;
    QObject::connect(
        &tray, &LinuxTrayController::quitRequested,
        &lifecycle,
        &GuardedLifecycleProbe::requestExplicitQuit,
        Qt::QueuedConnection);
    QSignalSpy guardedIntentSpy(
        &lifecycle,
        &GuardedLifecycleProbe::guardedIntentPublished);
    QSignalSpy displayApplySpy(
        &lifecycle,
        &GuardedLifecycleProbe::displayApplyRequested);

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(
        &watchdog, &QTimer::timeout,
        &application, [&application]() {
            application.exit(75);
        });
    watchdog.start(2000);

    QTimer::singleShot(0, &application, [bus]() mutable {
        QDBusMessage quit = QDBusMessage::createMethodCall(
            bus.baseService(),
            QStringLiteral("/StatusNotifierItem/Menu"),
            QStringLiteral("com.canonical.dbusmenu"),
            QStringLiteral("Event"));
        quit.setArguments({
            linuxtray::MenuModel::Quit,
            QStringLiteral("clicked"),
            QVariant::fromValue(
                QDBusVariant(QVariant(QString()))),
            static_cast<uint>(0),
        });
        bus.asyncCall(quit);
    });
    QTimer::singleShot(
        75, &application,
        [&application, &lifecycle, &guardedIntentSpy,
         &displayApplySpy, &window]() {
            if (guardedIntentSpy.count() != 1 ||
                lifecycle.pendingIntent() !=
                    GuardedLifecycleProbe::LeaveIntent::ExplicitQuit ||
                displayApplySpy.count() != 0 || !window.isVisible()) {
                application.exit(76);
                return;
            }
            lifecycle.approvePendingWithoutApply();
        });

    return application.exec();
}

int runWindowCloseProbe(
    QGuiApplication &application,
    bool hideToTrayOnClose, bool trayAvailable) {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(hideToTrayOnClose);
    windowChrome.setTrayAvailable(trayAvailable);
    TrayCloseFilter closeFilter(&windowChrome);
    window.installEventFilter(&closeFilter);
    window.show();

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(
        &watchdog, &QTimer::timeout,
        &application, [&application]() {
            application.exit(75);
        });
    watchdog.start(2000);

    QTimer::singleShot(0, &window, [&window]() {
        window.close();
    });
    return application.exec();
}

int runGuardedCloseProbe(QGuiApplication &application) {
    application.setQuitOnLastWindowClosed(false);

    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(true);
    windowChrome.setTrayAvailable(true);
    GuardedLifecycleProbe lifecycle(
        &application, &windowChrome);
    window.installEventFilter(&lifecycle);
    QSignalSpy guardedIntentSpy(
        &lifecycle,
        &GuardedLifecycleProbe::guardedIntentPublished);
    QSignalSpy displayApplySpy(
        &lifecycle,
        &GuardedLifecycleProbe::displayApplyRequested);
    window.show();

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(
        &watchdog, &QTimer::timeout,
        &application, [&application]() {
            application.exit(75);
        });
    watchdog.start(2000);

    QTimer::singleShot(0, &window, [&window]() {
        window.close();
    });
    QTimer::singleShot(
        25, &application,
        [&application, &lifecycle, &guardedIntentSpy,
         &displayApplySpy, &window, &windowChrome]() {
            if (guardedIntentSpy.count() != 1 ||
                lifecycle.pendingIntent() !=
                    GuardedLifecycleProbe::LeaveIntent::HideToTray ||
                lifecycle.closeEventCount() != 1 ||
                displayApplySpy.count() != 0 || !window.isVisible()) {
                application.exit(76);
                return;
            }

            lifecycle.approvePendingWithoutApply();
            if (window.isVisible() ||
                !windowChrome.hiddenToTray() ||
                displayApplySpy.count() != 0) {
                application.exit(77);
                return;
            }

            windowChrome.showWindow();
            windowChrome.setHideToTrayOnClose(false);
            window.close();
            QTimer::singleShot(
                25, &application,
                [&application, &lifecycle, &guardedIntentSpy,
                 &displayApplySpy, &window]() {
                    if (guardedIntentSpy.count() != 2 ||
                        lifecycle.pendingIntent() !=
                            GuardedLifecycleProbe::LeaveIntent::WindowQuit ||
                        lifecycle.closeEventCount() != 2 ||
                        displayApplySpy.count() != 0 ||
                        !window.isVisible()) {
                        application.exit(78);
                        return;
                    }

                    lifecycle.approvePendingWithoutApply();
                    if (lifecycle.pendingIntent() !=
                            GuardedLifecycleProbe::LeaveIntent::None ||
                        lifecycle.closeEventCount() != 3 ||
                        displayApplySpy.count() != 0 ||
                        window.isVisible()) {
                        application.exit(79);
                        return;
                    }

                    window.show();
                    window.close();
                    QTimer::singleShot(
                        25, &application,
                        [&application, &lifecycle,
                         &guardedIntentSpy, &displayApplySpy,
                         &window]() {
                            if (guardedIntentSpy.count() != 3 ||
                                lifecycle.pendingIntent() !=
                                    GuardedLifecycleProbe::LeaveIntent::WindowQuit ||
                                lifecycle.closeEventCount() != 4 ||
                                displayApplySpy.count() != 0 ||
                                !window.isVisible()) {
                                application.exit(80);
                                return;
                            }
                            application.exit(0);
                        });
                });
        });

    return application.exec();
}

}  // namespace

class LinuxTrayControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void menuModelHasStableContract();
    void menuModelFiltersProperties();
    void menuModelDispatchesOnlyClickActions();
    void menuLabelsAdvanceRevision();
    void dbusTypesMatchStatusNotifierSpecifications();
    void watcherLifecycleAndActions();
    void trayQuitWaitsForGuardApproval();
    void guardedCloseSeparatesHideAndWindowQuit();
    void windowChromeBridgesExplicitQuitApproval();
    void windowCloseWithQuitPolicyTerminatesGuiEventLoop();
    void windowCloseWithoutHostTerminatesGuiEventLoop();
    void autostartHidesWhenHostIsAlreadyAvailable();
    void autostartHidesWhenHostAppearsBeforeDeadline();
    void autostartShowsWhenHostDeadlineExpires();
    void autostartShowsImmediatelyWithQuitPolicy();
    void manualActivationAndHostLossRestoreWindow();
    void notificationsUseFreedesktopService();
    void noWatcherFallsBackToUnavailable();
};

void LinuxTrayControllerTests::initTestCase() {
    linuxtray::registerDBusTypes();
}

void LinuxTrayControllerTests::menuModelHasStableContract() {
    linuxtray::MenuModel model;

    QCOMPARE(
        model.children(linuxtray::MenuModel::Root),
        QList<int>({
            linuxtray::MenuModel::Open,
            linuxtray::MenuModel::Separator,
            linuxtray::MenuModel::Quit,
        }));
    QVERIFY(model.children(
        linuxtray::MenuModel::Open).isEmpty());

    const QVariantMap open = model.properties(
        linuxtray::MenuModel::Open);
    QCOMPARE(
        open.value(QStringLiteral("label")).toString(),
        QStringLiteral("Open"));
    QVERIFY(
        open.value(QStringLiteral("enabled")).toBool());
    QVERIFY(
        open.value(QStringLiteral("visible")).toBool());

    const QVariantMap separator = model.properties(
        linuxtray::MenuModel::Separator);
    QCOMPARE(
        separator.value(QStringLiteral("type")).toString(),
        QStringLiteral("separator"));

    const QVariantMap quit = model.properties(
        linuxtray::MenuModel::Quit);
    QCOMPARE(
        quit.value(QStringLiteral("label")).toString(),
        QStringLiteral("Quit"));
}

void LinuxTrayControllerTests::menuModelFiltersProperties() {
    linuxtray::MenuModel model;
    const QVariantMap filtered = model.properties(
        linuxtray::MenuModel::Open,
        {QStringLiteral("label")});
    QCOMPARE(filtered.size(), 1);
    QCOMPARE(
        filtered.value(QStringLiteral("label")).toString(),
        QStringLiteral("Open"));

    QVERIFY(model.properties(
        linuxtray::MenuModel::Open,
        {QStringLiteral("unknown")}).isEmpty());
}

void LinuxTrayControllerTests::
menuModelDispatchesOnlyClickActions() {
    linuxtray::MenuModel model;
    using Action = linuxtray::MenuModel::Action;

    QCOMPARE(
        model.actionForEvent(
            linuxtray::MenuModel::Open,
            QStringLiteral("clicked")),
        Action::Show);
    QCOMPARE(
        model.actionForEvent(
            linuxtray::MenuModel::Quit,
            QStringLiteral("clicked")),
        Action::Quit);
    QCOMPARE(
        model.actionForEvent(
            linuxtray::MenuModel::Separator,
            QStringLiteral("clicked")),
        Action::None);
    QCOMPARE(
        model.actionForEvent(
            linuxtray::MenuModel::Open,
            QStringLiteral("hovered")),
        Action::None);
}

void LinuxTrayControllerTests::menuLabelsAdvanceRevision() {
    linuxtray::MenuModel model;
    const quint32 initial = model.revision();

    model.setLabels(
        QStringLiteral("Open"),
        QStringLiteral("Quit"));
    QCOMPARE(model.revision(), initial);

    model.setLabels(
        QStringLiteral("Открыть"),
        QStringLiteral("Выйти"));
    QCOMPARE(model.revision(), initial + 1);
    QCOMPARE(
        model.properties(linuxtray::MenuModel::Open)
            .value(QStringLiteral("label")).toString(),
        QStringLiteral("Открыть"));
    QCOMPARE(
        model.properties(linuxtray::MenuModel::Quit)
            .value(QStringLiteral("label")).toString(),
        QStringLiteral("Выйти"));

    model.setLabels(QString(), QString());
    QCOMPARE(model.revision(), initial + 2);
    QCOMPARE(
        model.properties(linuxtray::MenuModel::Open)
            .value(QStringLiteral("label")).toString(),
        QStringLiteral("Open"));
}

void LinuxTrayControllerTests::
dbusTypesMatchStatusNotifierSpecifications() {
    const auto signature = [](QMetaType type) {
        const char *value =
            QDBusMetaType::typeToSignature(type);
        return QString::fromLatin1(value ? value : "");
    };

    QCOMPARE(
        signature(
            QMetaType::fromType<linuxtray::IconPixmap>()),
        QStringLiteral("(iiay)"));
    QCOMPARE(
        signature(
            QMetaType::fromType<
                linuxtray::IconPixmapList>()),
        QStringLiteral("a(iiay)"));
    QCOMPARE(
        signature(
            QMetaType::fromType<linuxtray::ToolTip>()),
        QStringLiteral("(sa(iiay)ss)"));
    QCOMPARE(
        signature(
            QMetaType::fromType<linuxtray::MenuLayout>()),
        QStringLiteral("(ia{sv}av)"));
    QCOMPARE(
        signature(
            QMetaType::fromType<
                linuxtray::MenuItemPropertiesList>()),
        QStringLiteral("a(ia{sv})"));
    QCOMPARE(
        signature(
            QMetaType::fromType<
                linuxtray::MenuItemsPropertiesRemovedList>()),
        QStringLiteral("a(ias)"));
    QCOMPARE(
        signature(
            QMetaType::fromType<
                linuxtray::MenuEventList>()),
        QStringLiteral("a(isvu)"));
}

void LinuxTrayControllerTests::
watcherLifecycleAndActions() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());

    MockStatusNotifierWatcher firstWatcher;
    QVERIFY(registerMockWatcher(bus, &firstWatcher));

    LinuxTrayController tray;
    QSignalSpy availableSpy(
        &tray, &LinuxTrayController::availableChanged);
    QSignalSpy showSpy(
        &tray, &LinuxTrayController::showRequested);
    QSignalSpy quitSpy(
        &tray, &LinuxTrayController::quitRequested);

    QTRY_VERIFY(tray.available());
    QTRY_COMPARE(firstWatcher.registrationCount(), 1);
    QCOMPARE(
        firstWatcher.registeredItem(),
        QStringLiteral("/StatusNotifierItem"));

    firstWatcher.setHostRegistered(false);
    QTRY_VERIFY(!tray.available());
    firstWatcher.setHostRegistered(true);
    QTRY_VERIFY(tray.available());

    QDBusMessage activate =
        QDBusMessage::createMethodCall(
            bus.baseService(),
            QStringLiteral("/StatusNotifierItem"),
            QStringLiteral(
                "org.kde.StatusNotifierItem"),
            QStringLiteral("Activate"));
    activate.setArguments({0, 0});
    auto activateReply = bus.asyncCall(activate);
    auto *activateWatcher =
        new QDBusPendingCallWatcher(activateReply, this);
    QTRY_COMPARE(showSpy.count(), 1);
    QTRY_VERIFY(activateWatcher->isFinished());
    QVERIFY(
        !QDBusPendingReply<>(*activateWatcher).isError());
    delete activateWatcher;

    QDBusMessage quit =
        QDBusMessage::createMethodCall(
            bus.baseService(),
            QStringLiteral("/StatusNotifierItem/Menu"),
            QStringLiteral("com.canonical.dbusmenu"),
            QStringLiteral("Event"));
    quit.setArguments({
        linuxtray::MenuModel::Quit,
        QStringLiteral("clicked"),
        QVariant::fromValue(
            QDBusVariant(QVariant(QString()))),
        static_cast<uint>(0),
    });
    auto quitReply = bus.asyncCall(quit);
    auto *quitWatcher =
        new QDBusPendingCallWatcher(quitReply, this);
    QTRY_COMPARE(quitSpy.count(), 1);
    QTRY_VERIFY(quitWatcher->isFinished());
    QVERIFY(
        !QDBusPendingReply<>(*quitWatcher).isError());
    delete quitWatcher;

    unregisterMockWatcher(bus);
    QTRY_VERIFY(!tray.available());

    MockStatusNotifierWatcher secondWatcher;
    QVERIFY(registerMockWatcher(bus, &secondWatcher));
    QTRY_VERIFY(tray.available());
    QTRY_COMPARE(secondWatcher.registrationCount(), 1);
    QVERIFY(availableSpy.count() >= 3);

    unregisterMockWatcher(bus);
    QTRY_VERIFY(!tray.available());
}

void LinuxTrayControllerTests::
trayQuitWaitsForGuardApproval() {
    const QString dbusRunSession =
        QStandardPaths::findExecutable(
            QStringLiteral("dbus-run-session"));
    QVERIFY2(
        !dbusRunSession.isEmpty(),
        "dbus-run-session is required for the tray quit probe");

    QProcess probe;
    probe.start(
        dbusRunSession,
        {QStringLiteral("--"),
         QCoreApplication::applicationFilePath(),
         kQuitProbeArgument});
    QVERIFY2(
        probe.waitForFinished(5000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(
        probe.exitCode(),
        0);
}

void LinuxTrayControllerTests::
guardedCloseSeparatesHideAndWindowQuit() {
    QProcess probe;
    probe.start(
        QCoreApplication::applicationFilePath(),
        {kGuardedCloseProbeArgument});
    QVERIFY2(
        probe.waitForFinished(5000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
}

void LinuxTrayControllerTests::
windowChromeBridgesExplicitQuitApproval() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    window.show();

    QSignalSpy requestSpy(
        &windowChrome,
        &WindowChromeController::explicitQuitRequested);
    QSignalSpy approvalSpy(
        &windowChrome,
        &WindowChromeController::explicitQuitApproved);

    windowChrome.requestExplicitQuit();
    QCOMPARE(requestSpy.count(), 1);
    QVERIFY(window.isVisible());
    QCOMPARE(approvalSpy.count(), 0);

    windowChrome.approveExplicitQuit();
    QCOMPARE(requestSpy.count(), 1);
    QCOMPARE(approvalSpy.count(), 1);
    QVERIFY(window.isVisible());
}

void LinuxTrayControllerTests::
windowCloseWithQuitPolicyTerminatesGuiEventLoop() {
    QProcess probe;
    probe.start(
        QCoreApplication::applicationFilePath(),
        {kQuitPolicyCloseProbeArgument});
    QVERIFY2(
        probe.waitForFinished(5000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
}

void LinuxTrayControllerTests::
windowCloseWithoutHostTerminatesGuiEventLoop() {
    QProcess probe;
    probe.start(
        QCoreApplication::applicationFilePath(),
        {kNoHostCloseProbeArgument});
    QVERIFY2(
        probe.waitForFinished(5000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
}

void LinuxTrayControllerTests::
autostartHidesWhenHostIsAlreadyAvailable() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(true);
    StartupVisibilityController visibility(
        &windowChrome, 100);

    visibility.setTrayAvailable(true);
    visibility.beginAutostart();

    QVERIFY(!visibility.autostartPending());
    QVERIFY(!window.isVisible());
    QVERIFY(windowChrome.hiddenToTray());
}

void LinuxTrayControllerTests::
autostartHidesWhenHostAppearsBeforeDeadline() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(true);
    StartupVisibilityController visibility(
        &windowChrome, 250);

    visibility.setTrayAvailable(false);
    visibility.beginAutostart();
    QVERIFY(visibility.autostartPending());

    QTimer::singleShot(
        10, &visibility, [&visibility]() {
            visibility.setTrayAvailable(true);
        });
    QTRY_VERIFY_WITH_TIMEOUT(
        !visibility.autostartPending(), 200);
    QVERIFY(!window.isVisible());
    QVERIFY(windowChrome.hiddenToTray());
}

void LinuxTrayControllerTests::
autostartShowsWhenHostDeadlineExpires() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(true);
    StartupVisibilityController visibility(
        &windowChrome, 20);

    visibility.setTrayAvailable(false);
    visibility.beginAutostart();

    QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 200);
    QVERIFY(!visibility.autostartPending());
    QVERIFY(!windowChrome.hiddenToTray());
}

void LinuxTrayControllerTests::
autostartShowsImmediatelyWithQuitPolicy() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(false);
    StartupVisibilityController visibility(
        &windowChrome, 100);

    visibility.setTrayAvailable(true);
    visibility.beginAutostart();

    QVERIFY(window.isVisible());
    QVERIFY(!visibility.autostartPending());
    QVERIFY(!windowChrome.hiddenToTray());
}

void LinuxTrayControllerTests::
manualActivationAndHostLossRestoreWindow() {
    QWindow window;
    WindowChromeController windowChrome;
    windowChrome.setWindow(&window);
    windowChrome.setHideToTrayOnClose(true);
    StartupVisibilityController visibility(
        &windowChrome, 100);

    visibility.setTrayAvailable(true);
    visibility.beginAutostart();
    QVERIFY(windowChrome.hiddenToTray());

    visibility.showWindow();
    QVERIFY(window.isVisible());
    QVERIFY(!windowChrome.hiddenToTray());

    visibility.beginAutostart();
    QVERIFY(windowChrome.hiddenToTray());
    visibility.setTrayAvailable(false);
    QVERIFY(window.isVisible());
    QVERIFY(!windowChrome.hiddenToTray());
}

void LinuxTrayControllerTests::
notificationsUseFreedesktopService() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());

    MockNotifications notifications;
    const auto flags =
        QDBusConnection::ExportAllSlots;
    QVERIFY(bus.registerObject(
        kNotificationsPath, &notifications, flags));
    QVERIFY(bus.registerService(
        kNotificationsService));

    LinuxTrayController tray;
    QSignalSpy failureSpy(
        &tray, &LinuxTrayController::notificationFailed);
    tray.showNotification(
        QStringLiteral("Connected"),
        QStringLiteral("PASE display session is ready"),
        1000);

    QTRY_COMPARE(notifications.notifyCount(), 1);
    QCOMPARE(
        notifications.lastSummary(),
        QStringLiteral("Connected"));
    QCOMPARE(
        notifications.lastBody(),
        QStringLiteral("PASE display session is ready"));
    QCOMPARE(failureSpy.count(), 0);

    bus.unregisterService(kNotificationsService);
    bus.unregisterObject(kNotificationsPath);
}

void LinuxTrayControllerTests::
noWatcherFallsBackToUnavailable() {
    LinuxTrayController tray;
    QVERIFY(!tray.available());
}

int main(int argc, char **argv) {
    qputenv(
        "QT_QPA_PLATFORM",
        QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    if (application.arguments().contains(
            kQuitProbeArgument)) {
        return runTrayQuitProbe(application);
    }
    if (application.arguments().contains(
            kQuitPolicyCloseProbeArgument)) {
        return runWindowCloseProbe(
            application, false, true);
    }
    if (application.arguments().contains(
            kNoHostCloseProbeArgument)) {
        return runWindowCloseProbe(
            application, true, false);
    }
    if (application.arguments().contains(
            kGuardedCloseProbeArgument)) {
        return runGuardedCloseProbe(application);
    }
    LinuxTrayControllerTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "linuxtraycontroller_tests.moc"
