#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QLocale>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTranslator>
#include <cstdlib>
#include "mainwindow.h"
#include "panorama/config.hpp"

namespace {

QString instanceSocketPath() {
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString baseDir = runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir;
    return QDir(baseDir).filePath("tryx-panorama-manager.instance");
}

bool notifyRunningInstance(const QString &path) {
    QLocalSocket probe;
    probe.connectToServer(path);
    if (!probe.waitForConnected(300)) {
        return false;
    }

    probe.write("show");
    probe.flush();
    probe.waitForBytesWritten(300);
    return true;
}

QString configuredLanguage() {
    auto config = panorama::ConfigManager::load_config();
    if (!config) {
        return "system";
    }
    return QString::fromStdString(config->language);
}

void saveLanguage(const QString &language) {
    panorama::Config config = panorama::ConfigManager::load_config().value_or(panorama::Config{});
    config.language = (language == "en" || language == "ru") ? language.toStdString() : "system";
    panorama::ConfigManager::save_config(config);
}

void applyLanguage(QApplication &app, QTranslator &translator, const QString &language) {
    app.removeTranslator(&translator);

    if (language == "en") {
        return;
    }

    const QLocale locale = language == "system" ? QLocale::system() : QLocale(language);
    if (locale.language() == QLocale::English) {
        return;
    }

    if (translator.load(locale, "tryx-panorama", "_", ":/i18n")) {
        app.installTranslator(&translator);
    }
}

} // namespace

int main(int argc, char *argv[]) {
    // Suppress GStreamer device enumeration spam
    setenv("GST_DEBUG", "0", 0);
    setenv("PIPEWIRE_LOG_LEVEL", "0", 0);

    QLoggingCategory::setFilterRules(
        "qt.multimedia.*=false\n"
        "qt.core.qfuture.*=false\n");

    QApplication app(argc, argv);
    app.setApplicationName("TRYX Panorama Manager");
    app.setOrganizationName("DXVSI");
    app.setWindowIcon(QIcon(":/tryx-panorama.png"));
    app.setDesktopFileName("tryx-panorama-manager");

    QTranslator translator;
    QString activeLanguage = configuredLanguage();
    applyLanguage(app, translator, activeLanguage);

    const QString socketPath = instanceSocketPath();
    if (notifyRunningInstance(socketPath)) {
        return 0;
    }

    QLocalServer::removeServer(socketPath);
    QLocalServer instanceServer;
    if (!instanceServer.listen(socketPath)) {
        qWarning() << "Failed to start single-instance server:"
                   << instanceServer.errorString();
    }

    auto languageHandler = [&](const QString &language) {
        activeLanguage = (language == "en" || language == "ru") ? language : "system";
        saveLanguage(activeLanguage);
        applyLanguage(app, translator, activeLanguage);
    };

    MainWindow window(activeLanguage, languageHandler);
    window.show();

    QObject::connect(&instanceServer, &QLocalServer::newConnection, &window, [&]() {
        QLocalSocket *connection = instanceServer.nextPendingConnection();
        if (connection) {
            connection->deleteLater();
        }

        window.show();
        window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        window.raise();
        window.activateWindow();
    });

    return app.exec();
}
