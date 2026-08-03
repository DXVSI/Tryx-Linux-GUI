#pragma once

#include <QString>
#include <QStringList>

class QLocalServer;

namespace quickbootstrap {

struct RuntimeBootstrapOptions {
    QString systemctlProgram = QStringLiteral("systemctl");
    QString developmentRuntimeProgram;
    QStringList developmentRuntimeArguments;
    QString installedRuntimeFallbackProgram;
    QStringList installedRuntimeFallbackArguments;
    int dbusCallTimeoutMs = 2000;
    int startupTimeoutMs = 8000;
    bool discoverDevelopmentRuntime = true;
    bool allowInstalledRuntimeFallback = true;
};

QString instanceSocketPath();
bool notifyRunningInstance(const QString &path);
bool listenForSingleInstance(QLocalServer *server,
                             const QString &path,
                             QString *errorMessage);
bool ensureRuntimeService(QString *errorMessage);
bool ensureRuntimeService(
    QString *errorMessage,
    const RuntimeBootstrapOptions &options);

}  // namespace quickbootstrap
