#include "appsettingscontroller.h"
#include "cachemanagementcontroller.h"
#include "devicemediaworkflowcontroller.h"
#include "firmwarecontroller.h"
#include "linuxtraycontroller.h"
#include "mediaeditorcontroller.h"
#include "mediapreviewcontroller.h"
#include "runtimebootstrap.h"
#include "runtimeclient.h"
#include "startupvisibilitycontroller.h"
#include "supportbundlecontroller.h"
#include "systemmetricsmodel.h"
#include "windowchromecontroller.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QVariantMap>
#include <QWindow>

#include <panorama/config.hpp>

#include <cstdio>

namespace {

void configureApplicationIdentity(QCoreApplication &app) {
    app.setApplicationName(QStringLiteral("TRYX Panorama Manager"));
    app.setApplicationVersion(QStringLiteral(TRYX_APP_VERSION));
    app.setOrganizationName(QStringLiteral("DXVSI"));
}

bool hasArgument(const QStringList &arguments,
                 const QString &value) {
    return arguments.contains(value);
}

QString configuredLanguage() {
    const auto config = panorama::ConfigManager::load_config();
    return config
        ? QString::fromStdString(config->language)
        : QStringLiteral("en");
}

void applyLanguage(QCoreApplication &app, QTranslator &translator,
                   const QString &language) {
    app.removeTranslator(&translator);
    if (language == QStringLiteral("en")) {
        return;
    }
    const QLocale locale =
        language == QStringLiteral("system")
        ? QLocale::system()
        : QLocale(language);
    if (locale.language() == QLocale::English) {
        return;
    }
    if (translator.load(
            locale, QStringLiteral("tryx-panorama"),
            QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        app.installTranslator(&translator);
    }
}

bool rawArgumentPresent(int argc, char *argv[],
                        const char *expected) {
    for (int index = 1; index < argc; ++index) {
        if (qstrcmp(argv[index], expected) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc > 1 &&
        qstrcmp(argv[1], "--internal-stage-copy") == 0) {
        QStringList helperArguments;
        for (int index = 2; index < argc; ++index) {
            helperArguments.append(
                QString::fromLocal8Bit(argv[index]));
        }
        return MediaPreviewController::runStageCopyHelper(
            helperArguments);
    }
    if (argc > 1 &&
        qstrcmp(
            argv[1],
            "--internal-export-device-media") == 0) {
        QStringList helperArguments;
        for (int index = 2; index < argc; ++index) {
            helperArguments.append(
                QString::fromLocal8Bit(argv[index]));
        }
        return DeviceMediaWorkflowController::runExportHelper(
            helperArguments);
    }
    if (rawArgumentPresent(argc, argv, "--version")) {
        std::fputs(
            "tryx-panorama-manager " TRYX_APP_VERSION "\n",
            stdout);
        return 0;
    }

    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    configureApplicationIdentity(app);
    app.setWindowIcon(
        QIcon(QStringLiteral(":/icons/tryx-panorama.png")));
    app.setDesktopFileName(
        QStringLiteral("tryx-panorama-manager"));

    const QStringList arguments = app.arguments();
    const bool smokeTest =
        hasArgument(arguments, QStringLiteral("--smoke-test"));
    const bool autostartRequested =
        hasArgument(arguments, QStringLiteral("--autostart"));

    QTranslator translator;
    applyLanguage(app, translator, configuredLanguage());

    const QString instanceSocketPath =
        quickbootstrap::instanceSocketPath();
    quickbootstrap::SingleInstanceGuard instanceGuard(
        instanceSocketPath);
    QLocalServer instanceServer;
    if (!smokeTest) {
        using quickbootstrap::SingleInstanceAcquireResult;

        const quickbootstrap::InstanceLaunchIntent launchIntent =
            autostartRequested
                ? quickbootstrap::InstanceLaunchIntent::Autostart
                : quickbootstrap::InstanceLaunchIntent::Manual;
        QString socketError;
        const SingleInstanceAcquireResult acquireResult =
            instanceGuard.acquire(
                &instanceServer, launchIntent, &socketError);
        if (acquireResult ==
            SingleInstanceAcquireResult::NotifiedExisting) {
            return 0;
        }
        if (acquireResult != SingleInstanceAcquireResult::Primary) {
            qCritical().noquote()
                << "Could not acquire the desktop client single-instance lease:"
                << socketError;
            return 1;
        }

        QString runtimeError;
        if (!quickbootstrap::ensureRuntimeService(&runtimeError)) {
            qCritical().noquote()
                << "Could not start the TRYX runtime:"
                << runtimeError;
            return 1;
        }
    }

    RuntimeClient runtime(smokeTest);
    SupportBundleController supportBundle(&runtime);
    MediaEditorController mediaEditor(&runtime);
    CacheManagementController cacheManagement(
        &runtime, &mediaEditor);
    DeviceMediaWorkflowController deviceMedia(
        &runtime, &mediaEditor);
    FirmwareController firmware;
    SystemMetricsModel systemMetrics;
    AppSettingsController settings(smokeTest);
    WindowChromeController windowChrome;
    StartupVisibilityController startupVisibility(
        &windowChrome);
    LinuxTrayController tray;

    windowChrome.setHideToTrayOnClose(
        settings.hideToTrayOnClose());
    QObject::connect(
        &settings, &AppSettingsController::closeBehaviorChanged,
        &windowChrome, [&settings, &windowChrome]() {
            windowChrome.setHideToTrayOnClose(
                settings.hideToTrayOnClose());
        });

    const auto updateTrayPresentation =
        [&tray, &runtime]() {
            tray.setLabels(
                LinuxTrayController::tr("Open"),
                LinuxTrayController::tr("Quit"));
            tray.setToolTip(
                QStringLiteral("TRYX Panorama Manager"),
                runtime.connectionStatus());
        };
    updateTrayPresentation();
    startupVisibility.setTrayAvailable(tray.available());
    QObject::connect(
        &tray, &LinuxTrayController::availableChanged,
        &startupVisibility,
        [&tray, &startupVisibility]() {
            startupVisibility.setTrayAvailable(
                tray.available());
        });
    QObject::connect(
        &tray, &LinuxTrayController::quitRequested,
        &windowChrome,
        &WindowChromeController::requestExplicitQuit,
        Qt::QueuedConnection);
    // Approval is emitted only after Main.qml has either found no dirty
    // display draft or resolved the guard. Exit directly so the normal
    // close-to-tray policy cannot intercept an explicit Quit.
    QObject::connect(
        &windowChrome, &WindowChromeController::explicitQuitApproved,
        &app,
        []() { QCoreApplication::exit(0); },
        Qt::QueuedConnection);
    QObject::connect(
        &runtime, &RuntimeClient::connectionChanged,
        &tray, updateTrayPresentation);

    QQmlApplicationEngine engine;
    QObject::connect(
        &settings, &AppSettingsController::languageChanged,
        &engine,
        [&app, &translator, &settings, &engine, &runtime, &firmware,
         &supportBundle, &cacheManagement,
         &updateTrayPresentation]() {
            applyLanguage(
                app, translator, settings.language());
            engine.retranslate();
            runtime.retranslate();
            firmware.retranslate();
            supportBundle.retranslate();
            cacheManagement.retranslate();
            updateTrayPresentation();
        });
    engine.setInitialProperties({
        {QStringLiteral("runtime"),
         QVariant::fromValue(static_cast<QObject *>(&runtime))},
        {QStringLiteral("mediaEditor"),
         QVariant::fromValue(static_cast<QObject *>(&mediaEditor))},
        {QStringLiteral("deviceMedia"),
         QVariant::fromValue(static_cast<QObject *>(&deviceMedia))},
        {QStringLiteral("firmware"),
         QVariant::fromValue(static_cast<QObject *>(&firmware))},
        {QStringLiteral("systemMetrics"),
         QVariant::fromValue(
             static_cast<QObject *>(&systemMetrics))},
        {QStringLiteral("settings"),
         QVariant::fromValue(static_cast<QObject *>(&settings))},
        {QStringLiteral("windowChrome"),
         QVariant::fromValue(
             static_cast<QObject *>(&windowChrome))},
        {QStringLiteral("supportBundle"),
         QVariant::fromValue(
             static_cast<QObject *>(&supportBundle))},
        {QStringLiteral("cacheManagement"),
         QVariant::fromValue(
             static_cast<QObject *>(&cacheManagement))},
        {QStringLiteral("quickSmokeTest"), smokeTest},
        {QStringLiteral("autostartRequested"),
         autostartRequested},
    });

    const QUrl entry(QStringLiteral("qrc:/qml/Main.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(2); },
        Qt::QueuedConnection);
    engine.load(entry);
    if (engine.rootObjects().isEmpty()) {
        return 2;
    }
    auto *rootWindow =
        qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    windowChrome.setWindow(rootWindow);

    QObject::connect(
        &tray, &LinuxTrayController::showRequested,
        &startupVisibility,
        &StartupVisibilityController::showWindow);

    if (autostartRequested && !smokeTest) {
        startupVisibility.beginAutostart();
    }
    const auto drainInstanceLaunchConnections =
        [&instanceServer, &app, &startupVisibility]() {
            quickbootstrap::drainPendingInstanceLaunchConnections(
                &instanceServer, &app,
                [&startupVisibility](
                    quickbootstrap::InstanceLaunchIntent intent) {
                    if (quickbootstrap::
                            instanceLaunchIntentRequestsWindow(intent)) {
                        startupVisibility.showWindow();
                    }
                });
        };
    QObject::connect(
        &instanceServer, &QLocalServer::newConnection, &app,
        drainInstanceLaunchConnections);
    // The server starts listening before the QML engine is ready. A launch
    // can therefore already be pending by the time this signal handler is
    // installed.
    drainInstanceLaunchConnections();
    if (smokeTest) {
        QTimer::singleShot(0, &app, [&app]() { app.exit(0); });
    }
    return app.exec();
}
