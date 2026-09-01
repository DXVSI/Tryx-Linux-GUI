#pragma once

#include <QByteArray>
#include <QDBusConnection>
#include <QStringList>

#ifdef TRYX_CLI_TESTING
#include <QDBusMessage>
#endif

namespace tryx::cli {

struct RunResult {
    int exitCode = 70;
    QByteArray standardOutput;
    QByteArray standardError;
};

QByteArray helpText();
QByteArray versionText();

RunResult runWithoutSessionBus(
    const QStringList &arguments);

RunResult run(
    const QStringList &arguments,
    const QDBusConnection &bus,
    int dbusDeadlineMs = 5000);

#ifdef TRYX_CLI_TESTING
QDBusMessage runtimeMethodCallForTesting(
    const QString &owner,
    const QString &method);
#endif

}  // namespace tryx::cli
