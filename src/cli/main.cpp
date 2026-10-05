#include "sessionbusguard.h"
#include "tryxclirunner.h"

#include <QCoreApplication>
#include <QDBusConnection>

#include <cstdio>

namespace {

bool writeAll(FILE *stream, const QByteArray &bytes) {
    if (bytes.isEmpty()) {
        return true;
    }
    const size_t expected = static_cast<size_t>(bytes.size());
    return std::fwrite(
               bytes.constData(), 1, expected, stream) == expected &&
        std::fflush(stream) == 0;
}

QStringList commandArguments(int argc, char *argv[]) {
    QStringList arguments;
    arguments.reserve(argc > 1 ? argc - 1 : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.append(QString::fromLocal8Bit(argv[index]));
    }
    return arguments;
}

}  // namespace

int main(int argc, char *argv[]) {
    const QStringList arguments =
        commandArguments(argc, argv);
    if (arguments == QStringList{QStringLiteral("--help")}) {
        return writeAll(stdout, tryx::cli::helpText()) ? 0 : 70;
    }
    if (arguments == QStringList{QStringLiteral("--version")}) {
        return writeAll(stdout, tryx::cli::versionText()) ? 0 : 70;
    }

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("tryx"));
    app.setApplicationVersion(
        QStringLiteral(TRYX_APP_VERSION));

    const QByteArray sessionBusAddress =
        qgetenv("DBUS_SESSION_BUS_ADDRESS");
    const tryx::cli::RunResult result =
        tryx::cli::localSessionBusAddressIsSafe(
            sessionBusAddress)
        ? tryx::cli::run(
              arguments, QDBusConnection::sessionBus())
        : tryx::cli::runWithoutSessionBus(arguments);
    const bool stdoutWritten =
        writeAll(stdout, result.standardOutput);
    const bool stderrWritten =
        writeAll(stderr, result.standardError);
    return stdoutWritten && stderrWritten
        ? result.exitCode
        : 70;
}
