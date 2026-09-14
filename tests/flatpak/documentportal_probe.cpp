#include "devicemediaworkflowcontroller.h"
#include "mediapreviewcontroller.h"
#include "packagingcontext.h"
#include "runtimecontract.h"
#include "supportbundle.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>

#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using Descriptors = QList<QDBusUnixFileDescriptor>;
Q_DECLARE_METATYPE(Descriptors)

namespace {
bool check(bool condition, const QString &description) {
    if (!condition) std::fprintf(stderr, "%s\n", qPrintable(description));
    return condition;
}
bool privateFile(const QString &path, const QByteArray &expected) {
    struct stat status {};
    QFile file(path);
    return check(::lstat(QFile::encodeName(path).constData(), &status) == 0 &&
                 S_ISREG(status.st_mode) && status.st_uid == ::getuid() &&
                 status.st_nlink == 1 && (status.st_mode & 0777) == 0600 &&
                 file.open(QIODevice::ReadOnly) && file.readAll() == expected,
                 QStringLiteral("Invalid private file: ") + path);
}

int exercise(const QString &input, const QString &output) {
    const QUrl selectedFolder = tryx::packaging::resolveDocumentsPortalAlias(QUrl::fromLocalFile(output));
    // Isolate all inbox/spool paths without modifying the user's application data.
    QTemporaryDir runtime;
    if (!runtime.isValid()) return 2;
    QTemporaryDir applicationParent;
    if (!applicationParent.isValid() ||
        !QFile::link(applicationParent.path(), runtime.filePath(QStringLiteral("app")))) return 2;
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    QFile source(input);
    if (!source.open(QIODevice::ReadOnly)) return 3;
    const QByteArray payload = source.readAll();
    const QFileInfo sourceInfo(input);
    const QString inbox = tryxRuntimeMediaInboxPath();
    const QString outbox = tryxRuntimeDeviceMediaOutboxPath();
    for (const QString &directory : {tryx::packaging::runtimeDirectory(),
                                     QFileInfo(inbox).absolutePath(), inbox, outbox}) {
        if (!QDir().mkpath(directory) || !QFile::setPermissions(directory,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return 4;
    }
    const QString snapshot = QDir(inbox).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".png"));
    if (!check(MediaPreviewController::runStageCopyHelper({input, snapshot,
        QString::number(sourceInfo.size()),
        QString::number(sourceInfo.lastModified().toMSecsSinceEpoch())}) == 0,
        QStringLiteral("Portal source snapshot failed")) || !privateFile(snapshot, payload)) return 5;

    QStorageInfo oldStorage(output);
    oldStorage.refresh();
    struct statvfs storage {};
    if (::statvfs(QFile::encodeName(output).constData(), &storage) != 0) {
        qCritical() << "Selected directory statvfs failed:" << output << std::strerror(errno);
        return 6;
    }
    std::fprintf(stderr, "Storage: Qt mount refresh=%lld selected directory=%llu\n",
        static_cast<long long>(oldStorage.bytesAvailable()),
        static_cast<unsigned long long>(quint64(storage.f_bavail) * storage.f_frsize));
    const QString recovered = QDir(outbox).filePath(QStringLiteral("fixture.h264"));
    if (!QFile::copy(snapshot, recovered)) return 7;
    // The controller resolves the selected folder before launching the helper.
    // In an installed Flatpak /run/user/<uid>/doc aliases /run/flatpak/doc.
    const QString canonicalOutput = QFileInfo(output).canonicalFilePath();
    if (canonicalOutput.isEmpty()) return 7;
    const QString destination = QDir(canonicalOutput).filePath(QStringLiteral("fixture.h264"));
    QStringList arguments{recovered, destination, QString::number(payload.size()),
        QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()),
        QStringLiteral("0")};
    const int result = DeviceMediaWorkflowController::runExportHelper(arguments);
    if (!check(result == 0, QStringLiteral("Portal media export failed: %1").arg(result)) ||
        !privateFile(destination, payload)) return 8;
    if (!check(DeviceMediaWorkflowController::runExportHelper(arguments) == 3,
               QStringLiteral("Export unexpectedly clobbered an existing file"))) return 9;
    arguments[4] = QStringLiteral("1");
    if (DeviceMediaWorkflowController::runExportHelper(arguments) != 0 ||
        !privateFile(destination, payload)) return 10;
    const QString reportName = tryx::support_bundle::generatedFileName();
    const QByteArray report("{\"schema_version\":1}\n");
    const auto written = tryx::support_bundle::writeNewReport(
        selectedFolder, reportName, report);
    if (!check(written.ok(), written.message) || !privateFile(written.path, report)) return 11;
    if (tryx::support_bundle::writeNewReport(selectedFolder, reportName, report).ok())
        return 12;
    for (const QString &name : QDir(output).entryList(QDir::Files | QDir::Hidden)) {
        if (!check(name == QStringLiteral("fixture.h264") || name == reportName,
                   QStringLiteral("Unexpected temporary file: ") + name)) return 13;
    }
    std::fputs("PASS: runtime app alias, private source snapshot, media export, overwrite/no-clobber, support report\n", stderr);
    return 0;
}

int fixture(bool usePortal, const QString &appDirectory, const QString &executable) {
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    const QString input = directory.filePath(QStringLiteral("fixture.png"));
    const QString output = directory.filePath(QStringLiteral("output"));
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::blue);
    if (!image.save(input) || !QDir().mkdir(output)) return 3;
    if (!usePortal) return exercise(input, output);

    // Explicit opt-in integration probe: grants ONLY this newly-created fixture,
    // never enumerates or changes the user's existing document permissions.
    const auto bus = QDBusConnection::sessionBus();
    QStringList ids;
    const auto request = [&](const QString &method, const QVariantList &arguments) {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Documents"),
            QStringLiteral("/org/freedesktop/portal/documents"),
            QStringLiteral("org.freedesktop.portal.Documents"), method);
        message.setArguments(arguments);
        const auto reply = bus.call(message, QDBus::Block, 5000);
        std::fprintf(stderr, "Documents.%s: type=%d args=%lld signature=%s error=%s\n",
            qPrintable(method), int(reply.type()), static_cast<long long>(reply.arguments().size()),
            qPrintable(reply.signature()), qPrintable(reply.errorMessage()));
        return reply;
    };
    const auto cleanup = qScopeGuard([&]() {
        for (const QString &id : ids) {
            const auto reply = request(QStringLiteral("Delete"), {id});
            if (reply.type() == QDBusMessage::ErrorMessage)
                qCritical() << "Could not revoke fixture document" << id << reply.errorMessage();
        }
    });
    const auto mountReply = request(QStringLiteral("GetMountPoint"), {});
    if (mountReply.type() != QDBusMessage::ReplyMessage || mountReply.arguments().size() != 1)
        return 4;
    QByteArray mount = mountReply.arguments().first().toByteArray();
    if (mount.endsWith('\0')) mount.chop(1);
    qDBusRegisterMetaType<Descriptors>();
    QStringList paths;
    for (const auto &entry : {qMakePair(input, false), qMakePair(output, true)}) {
        // Documents rejects O_PATH|O_NOFOLLOW. A readable FD preserves the
        // no-symlink precondition and proves access to this owned fixture.
        const int fd = ::open(QFile::encodeName(entry.first).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return 5;
        const Descriptors descriptors{QDBusUnixFileDescriptor(fd)};
        ::close(fd);
        const auto reply = request(QStringLiteral("AddFull"), {
            QVariant::fromValue(descriptors), uint(entry.second ? 8 : 0),
            tryx::packaging::appId(), entry.second ? QStringList{"read", "write"} : QStringList{"read"}});
        if (!check(reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 2,
                   reply.errorMessage())) return 6;
        const QStringList added = qdbus_cast<QStringList>(reply.arguments().first());
        if (added.size() != 1 || added.first().isEmpty()) return 7;
        ids.append(added.first());
        paths.append(QFile::decodeName(mount) + '/' + ids.last() + '/' + QFileInfo(entry.first).fileName());
    }
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    if (appDirectory.isEmpty()) {
        // The host's per-app FUSE view enforces exactly the same document grants.
        for (QString &path : paths)
            path.replace(QFile::decodeName(mount) + '/', QFile::decodeName(mount) +
                         QStringLiteral("/by-app/") + tryx::packaging::appId() + '/');
        child.start(executable, {QStringLiteral("--exercise"), paths.at(0), paths.at(1)});
    } else {
        child.start(QStringLiteral("flatpak"), {QStringLiteral("build"),
            QStringLiteral("--unshare=network"), QStringLiteral("--nodevice=all"),
            QStringLiteral("--nosocket=session-bus"), QStringLiteral("--nosocket=system-bus"),
            QStringLiteral("--nofilesystem=host"), QStringLiteral("--filesystem=") + executable + ":ro",
            appDirectory, executable, QStringLiteral("--exercise"), paths.at(0), paths.at(1)});
    }
    if (!child.waitForStarted(3000)) return 8;
    if (!child.waitForFinished(20000)) {
        child.kill();
        child.waitForFinished(3000);
        return 9;
    }
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 10;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.value(1) == QStringLiteral("--exercise") && args.size() == 4)
        return exercise(args.at(2), args.at(3));
    if (args.value(1) == QStringLiteral("--fixture-only") && args.size() == 2)
        return fixture(false, {}, app.applicationFilePath());
    if (args.value(1) == QStringLiteral("--portal-fixture") && args.size() <= 3)
        return fixture(true, args.value(2), app.applicationFilePath());
    qCritical() << "Use --fixture-only or explicitly --portal-fixture [flatpak-build-directory]. No device I/O.";
    return 2;
}
