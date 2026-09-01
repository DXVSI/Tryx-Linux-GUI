#include "runtimebootstrap.h"

#include "runtimecontract.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <utility>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace quickbootstrap {
namespace {

constexpr int kProcessStartTimeoutMs = 2000;
constexpr int kInstanceLaunchReadTimeoutMs = 1000;
constexpr int kInstanceOwnerStartupWaitMs = 1000;
constexpr int kInstanceOwnerStartupRetryMs = 25;
constexpr qsizetype kMaxInstanceLaunchPayloadBytes = 32;
const QByteArray kInstanceLaunchAcknowledgement =
    QByteArrayLiteral("accepted\n");
thread_local bool gRuntimeDirectoryOverrideSet = false;
thread_local QString gRuntimeDirectoryOverride;
thread_local testing::AfterLeaseAcquiredBeforeSocketCleanupHook
    gAfterLeaseAcquiredBeforeSocketCleanupHook = nullptr;
thread_local testing::AfterPreviousLeaseProbeHook
    gAfterPreviousLeaseProbeHook = nullptr;
thread_local testing::BeforeStaleSocketExchangeHook
    gBeforeStaleSocketExchangeHook = nullptr;
thread_local testing::AfterStaleSocketExchangeHook
    gAfterStaleSocketExchangeHook = nullptr;
thread_local testing::AfterNativeSocketBoundHook
    gAfterNativeSocketBoundHook = nullptr;
thread_local testing::AfterRuntimeDirectoryPinnedHook
    gAfterRuntimeDirectoryPinnedHook = nullptr;
std::atomic<quint64> gReplacementSocketSequence{0};

enum class InstanceNotificationResult {
    NoOwner,
    Delivered,
    UnacknowledgedOwner,
};

class PendingInstanceLaunchReader final : public QObject {
public:
    PendingInstanceLaunchReader(
        QLocalSocket *socket,
        InstanceLaunchIntentHandler handler,
        QObject *parent)
        : QObject(parent), socket_(socket), handler_(std::move(handler)) {
        socket_->setParent(this);
        QObject::connect(
            socket_, &QLocalSocket::readyRead, this,
            [this]() { collectPayload(); });
        QObject::connect(
            socket_, &QLocalSocket::disconnected, this,
            [this]() {
                if (finished_) {
                    deleteLater();
                } else {
                    reject();
                }
            });
        QTimer::singleShot(
            kInstanceLaunchReadTimeoutMs, this,
            [this]() { reject(); });

        collectPayload();
        if (socket_->state() == QLocalSocket::UnconnectedState) {
            reject();
        }
    }

private:
    void collectPayload() {
        if (finished_) {
            return;
        }
        const qint64 remainingCapacity =
            kMaxInstanceLaunchPayloadBytes + 1 - payload_.size();
        if (remainingCapacity <= 0) {
            reject();
            return;
        }
        payload_.append(socket_->read(remainingCapacity));
        const qsizetype separator = payload_.indexOf('\n');
        if (separator < 0) {
            if (payload_.size() > kMaxInstanceLaunchPayloadBytes) {
                reject();
            }
            return;
        }
        if (separator > kMaxInstanceLaunchPayloadBytes) {
            reject();
            return;
        }
        const QByteArray framedPayload = payload_.first(separator);
        const std::optional<InstanceLaunchIntent> intent =
            parseInstanceLaunchIntentPayload(framedPayload);
        if (!intent.has_value() || !handler_) {
            reject();
            return;
        }

        finished_ = true;
        handler_(*intent);
        if (socket_->write(kInstanceLaunchAcknowledgement) !=
            kInstanceLaunchAcknowledgement.size()) {
            socket_->abort();
            deleteLater();
            return;
        }
        socket_->flush();
        socket_->disconnectFromServer();
        if (socket_->state() == QLocalSocket::UnconnectedState) {
            deleteLater();
        }
    }

    void reject() {
        if (finished_) {
            return;
        }
        finished_ = true;
        socket_->abort();
        deleteLater();
    }

    QLocalSocket *socket_ = nullptr;
    InstanceLaunchIntentHandler handler_;
    QByteArray payload_;
    bool finished_ = false;
};

InstanceNotificationResult notifyRunningInstanceWithResult(
    const QString &path,
    InstanceLaunchIntent intent) {
    const QByteArray payload = instanceLaunchIntentPayload(intent);
    if (path.isEmpty() || payload.isEmpty()) {
        return InstanceNotificationResult::NoOwner;
    }

    QLocalSocket probe;
    probe.connectToServer(path);
    if (!probe.waitForConnected(300)) {
        return InstanceNotificationResult::NoOwner;
    }

    const QByteArray frame = payload + '\n';
    if (probe.write(frame) != frame.size() || !probe.flush() ||
        (probe.bytesToWrite() > 0 &&
         !probe.waitForBytesWritten(300))) {
        probe.abort();
        return InstanceNotificationResult::UnacknowledgedOwner;
    }
    if (!probe.waitForReadyRead(300)) {
        probe.abort();
        return InstanceNotificationResult::UnacknowledgedOwner;
    }
    QByteArray acknowledgement = probe.readAll();
    while (acknowledgement.size() <
               kInstanceLaunchAcknowledgement.size() &&
           probe.waitForReadyRead(50)) {
        acknowledgement.append(probe.readAll());
    }
    probe.disconnectFromServer();
    if (probe.state() != QLocalSocket::UnconnectedState) {
        probe.waitForDisconnected(300);
    }
    return acknowledgement == kInstanceLaunchAcknowledgement
        ? InstanceNotificationResult::Delivered
        : InstanceNotificationResult::UnacknowledgedOwner;
}

struct SocketIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

bool inspectSocketIdentity(const QString &path,
                           SocketIdentity *identity) {
#if defined(Q_OS_LINUX)
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0 ||
        !S_ISSOCK(status.st_mode) ||
        status.st_uid != ::getuid()) {
        return false;
    }
    identity->device = status.st_dev;
    identity->inode = status.st_ino;
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(identity);
    return false;
#endif
}

bool inspectSocketIdentityAt(int directoryDescriptor,
                             const QByteArray &leafName,
                             SocketIdentity *identity,
                             mode_t *permissions = nullptr) {
#if defined(Q_OS_LINUX)
    struct stat status {};
    if (::fstatat(
            directoryDescriptor, leafName.constData(), &status,
            AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(status.st_mode) ||
        status.st_uid != ::getuid()) {
        return false;
    }
    identity->device = status.st_dev;
    identity->inode = status.st_ino;
    if (permissions) {
        *permissions = status.st_mode & 0777;
    }
    return true;
#else
    Q_UNUSED(directoryDescriptor);
    Q_UNUSED(leafName);
    Q_UNUSED(identity);
    Q_UNUSED(permissions);
    return false;
#endif
}

bool sameSocketIdentity(const SocketIdentity &left,
                        const SocketIdentity &right) {
    return left.device == right.device &&
           left.inode == right.inode;
}

enum class NativeSocketBindResult {
    Bound,
    AddressInUse,
    Failed,
};

struct NativeBoundSocket {
    int descriptor = -1;
    SocketIdentity identity;
};

void closeNativeBoundSocket(NativeBoundSocket *socket) {
#if defined(Q_OS_LINUX)
    if (socket->descriptor >= 0) {
        ::close(socket->descriptor);
        socket->descriptor = -1;
    }
#else
    Q_UNUSED(socket);
#endif
}

NativeSocketBindResult bindNativeUnixSocket(
    const QString &path,
    NativeBoundSocket *bound,
    QString *errorMessage) {
#if defined(Q_OS_LINUX)
    const QByteArray encodedPath = QFile::encodeName(path);
    if (encodedPath.isEmpty() || encodedPath.contains('\0') ||
        encodedPath.size() >=
            static_cast<qsizetype>(sizeof(sockaddr_un::sun_path))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The single-instance socket path is invalid");
        }
        return NativeSocketBindResult::Failed;
    }

    const int descriptor = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
        0);
    if (descriptor < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not create the single-instance socket");
        }
        return NativeSocketBindResult::Failed;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(
        address.sun_path, encodedPath.constData(),
        static_cast<size_t>(encodedPath.size() + 1));
    const socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) +
        encodedPath.size() + 1);
    if (::bind(
            descriptor,
            reinterpret_cast<const sockaddr *>(&address),
            addressLength) != 0) {
        const int bindError = errno;
        ::close(descriptor);
        if (bindError == EADDRINUSE) {
            return NativeSocketBindResult::AddressInUse;
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not bind the single-instance socket: %1")
                    .arg(QString::fromLocal8Bit(
                        std::strerror(bindError)));
        }
        return NativeSocketBindResult::Failed;
    }

    SocketIdentity identity;
    if (!inspectSocketIdentity(path, &identity) ||
        ::chmod(encodedPath.constData(), S_IRUSR | S_IWUSR) != 0 ||
        ::listen(descriptor, SOMAXCONN) != 0) {
        ::close(descriptor);
        SocketIdentity cleanupIdentity;
        if (inspectSocketIdentity(path, &cleanupIdentity) &&
            sameSocketIdentity(identity, cleanupIdentity)) {
            ::unlink(encodedPath.constData());
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not prepare the single-instance socket");
        }
        return NativeSocketBindResult::Failed;
    }
    SocketIdentity restrictedIdentity;
    struct stat restrictedStatus {};
    if (::lstat(encodedPath.constData(), &restrictedStatus) != 0 ||
        !S_ISSOCK(restrictedStatus.st_mode) ||
        restrictedStatus.st_uid != ::getuid() ||
        (restrictedStatus.st_mode & 0777) !=
            (S_IRUSR | S_IWUSR) ||
        !inspectSocketIdentity(path, &restrictedIdentity) ||
        !sameSocketIdentity(identity, restrictedIdentity)) {
        ::close(descriptor);
        SocketIdentity cleanupIdentity;
        if (inspectSocketIdentity(path, &cleanupIdentity) &&
            sameSocketIdentity(identity, cleanupIdentity)) {
            ::unlink(encodedPath.constData());
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The single-instance socket changed while it was prepared");
        }
        return NativeSocketBindResult::Failed;
    }

    bound->descriptor = descriptor;
    bound->identity = identity;
    return NativeSocketBindResult::Bound;
#else
    Q_UNUSED(path);
    Q_UNUSED(bound);
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Native single-instance sockets are unavailable");
    }
    return NativeSocketBindResult::Failed;
#endif
}

bool adoptNativeSocket(QLocalServer *server,
                       NativeBoundSocket *bound,
                       QString *errorMessage) {
    if (!server->listen(bound->descriptor)) {
        if (errorMessage) {
            *errorMessage = server->errorString();
        }
        closeNativeBoundSocket(bound);
        return false;
    }
    bound->descriptor = -1;
    return true;
}

bool unlinkSocketIfMatchesAt(
    int directoryDescriptor,
    const QByteArray &leafName,
    const SocketIdentity &expected) {
#if defined(Q_OS_LINUX)
    SocketIdentity actual;
    return inspectSocketIdentityAt(
               directoryDescriptor, leafName, &actual) &&
           sameSocketIdentity(expected, actual) &&
           ::unlinkat(
               directoryDescriptor, leafName.constData(), 0) == 0;
#else
    Q_UNUSED(directoryDescriptor);
    Q_UNUSED(leafName);
    Q_UNUSED(expected);
    return false;
#endif
}

bool exchangeInReplacementSocket(
    int directoryDescriptor,
    const QByteArray &socketLeafName,
    const QByteArray &replacementLeafName,
    const SocketIdentity &expectedStale,
    const SocketIdentity &expectedReplacement,
    const QString &reportedPath,
    QString *errorMessage) {
#if defined(Q_OS_LINUX) && defined(SYS_renameat2)
    if (gBeforeStaleSocketExchangeHook) {
        gBeforeStaleSocketExchangeHook(reportedPath);
    }
    if (::syscall(
            SYS_renameat2,
            directoryDescriptor, socketLeafName.constData(),
            directoryDescriptor, replacementLeafName.constData(),
            RENAME_EXCHANGE) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not atomically publish the replacement single-instance socket");
        }
        return false;
    }

    if (gAfterStaleSocketExchangeHook) {
        gAfterStaleSocketExchangeHook(reportedPath);
    }

    SocketIdentity publishedIdentity;
    SocketIdentity isolatedIdentity;
    const bool publishedInspected = inspectSocketIdentityAt(
        directoryDescriptor, socketLeafName,
        &publishedIdentity);
    const bool isolatedInspected = inspectSocketIdentityAt(
        directoryDescriptor, replacementLeafName,
        &isolatedIdentity);
    const bool publishedReplacement =
        publishedInspected && sameSocketIdentity(
            expectedReplacement, publishedIdentity);
    const bool isolatedStale =
        isolatedInspected && sameSocketIdentity(
            expectedStale, isolatedIdentity);
    if (!publishedReplacement || !isolatedStale) {
        // A winner can replace the stale canonical endpoint after the final
        // precondition check but immediately before RENAME_EXCHANGE. In that
        // case our replacement is now canonical and the live winner is
        // isolated under replacementLeafName. Exchange the two exact leaves
        // back before closing our descriptor, then remove only our returned
        // replacement. This is the bounded single-racer rollback; it is not a
        // security boundary against a process with the same UID continually
        // mutating the directory.
        if (publishedReplacement && isolatedInspected &&
            !isolatedStale) {
            if (::syscall(
                    SYS_renameat2,
                    directoryDescriptor,
                    socketLeafName.constData(),
                    directoryDescriptor,
                    replacementLeafName.constData(),
                    RENAME_EXCHANGE) == 0) {
                SocketIdentity restoredIdentity;
                SocketIdentity returnedReplacementIdentity;
                if (inspectSocketIdentityAt(
                        directoryDescriptor, socketLeafName,
                        &restoredIdentity) &&
                    sameSocketIdentity(
                        isolatedIdentity, restoredIdentity) &&
                    inspectSocketIdentityAt(
                        directoryDescriptor, replacementLeafName,
                        &returnedReplacementIdentity) &&
                    sameSocketIdentity(
                        expectedReplacement,
                        returnedReplacementIdentity)) {
                    unlinkSocketIfMatchesAt(
                        directoryDescriptor,
                        replacementLeafName,
                        expectedReplacement);
                }
            }
        } else if (isolatedStale) {
            // The post-exchange test hook can replace our canonical endpoint.
            // Keep that foreign winner and remove only the isolated stale leaf.
            unlinkSocketIfMatchesAt(
                directoryDescriptor,
                replacementLeafName,
                expectedStale);
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The single-instance socket changed during atomic replacement");
        }
        return false;
    }

    if (!unlinkSocketIfMatchesAt(
            directoryDescriptor, replacementLeafName,
            expectedStale)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Could not remove the isolated stale single-instance socket");
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(directoryDescriptor);
    Q_UNUSED(socketLeafName);
    Q_UNUSED(replacementLeafName);
    Q_UNUSED(expectedStale);
    Q_UNUSED(expectedReplacement);
    Q_UNUSED(reportedPath);
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "Atomic stale single-instance socket cleanup is unavailable");
    }
    return false;
#endif
}

struct DevelopmentRuntimeSelection {
    bool requested = false;
    QString program;
    QString error;
};

struct RuntimeUnitState {
    QString loadState;
    QString activeState;
    QString subState;
    QString job;
    quint64 mainPid = 0;
};

enum class RuntimeApiStatus {
    Compatible,
    VersionMismatch,
    Unavailable,
};

class StartedRuntimeProcess final {
public:
    StartedRuntimeProcess() = default;
    ~StartedRuntimeProcess() {
#if defined(Q_OS_LINUX)
        if (pidFd_ >= 0) {
            ::close(pidFd_);
        }
#endif
    }

    StartedRuntimeProcess(const StartedRuntimeProcess &) = delete;
    StartedRuntimeProcess &operator=(const StartedRuntimeProcess &) = delete;

    void capture(qint64 pid, const QString &executable) {
        pid_ = pid;
        executable_ = executable;
        startTime_ = processStartTime(pid_);
#if defined(Q_OS_LINUX) && defined(SYS_pidfd_open)
        pidFd_ = static_cast<int>(
            ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid_), 0));
#endif
    }

    qint64 pid() const { return pid_; }

    bool requestTermination() const {
        if (pid_ <= 0) {
            return false;
        }
#if defined(Q_OS_LINUX) && defined(SYS_pidfd_send_signal)
        if (pidFd_ >= 0) {
            return ::syscall(SYS_pidfd_send_signal, pidFd_, SIGTERM,
                             nullptr, 0) == 0 ||
                   errno == ESRCH;
        }
#endif
#if defined(Q_OS_LINUX)
        if (startTime_.isEmpty() ||
            processStartTime(pid_) != startTime_ ||
            QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid_))
                    .canonicalFilePath() != executable_) {
            return false;
        }
        return ::kill(static_cast<pid_t>(pid_), SIGTERM) == 0 ||
               errno == ESRCH;
#else
        return false;
#endif
    }

private:
    static QByteArray processStartTime(qint64 pid) {
#if defined(Q_OS_LINUX)
        QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!statFile.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray stat = statFile.readAll().trimmed();
        const qsizetype commandEnd = stat.lastIndexOf(')');
        if (commandEnd < 0 || commandEnd + 2 >= stat.size()) {
            return {};
        }
        const QList<QByteArray> fields =
            stat.mid(commandEnd + 2).split(' ');
        // The list starts at procfs field 3 (state); starttime is field 22.
        return fields.size() > 19 ? fields.at(19) : QByteArray{};
#else
        Q_UNUSED(pid);
        return {};
#endif
    }

    qint64 pid_ = -1;
    QString executable_;
    QByteArray startTime_;
#if defined(Q_OS_LINUX)
    int pidFd_ = -1;
#endif
};

QString runtimeServiceOwner() {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        return {};
    }
    const QDBusReply<QString> reply =
        bus.interface()->serviceOwner(tryxRuntimeServiceName());
    return reply.isValid() ? reply.value() : QString{};
}

void stopStartedRuntime(const StartedRuntimeProcess &process) {
    const QString owner = runtimeServiceOwner();
    if (!owner.isEmpty()) {
        const QDBusConnection bus = QDBusConnection::sessionBus();
        const QDBusReply<uint> pidReply =
            bus.interface()->servicePid(tryxRuntimeServiceName());
        if (!pidReply.isValid() ||
            static_cast<qint64>(pidReply.value()) != process.pid()) {
            return;
        }
    }
    if (!process.requestTermination()) {
        return;
    }

    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 1000) {
        const QString currentOwner = runtimeServiceOwner();
        if (currentOwner.isEmpty()) {
            return;
        }
        const QDBusReply<uint> pidReply =
            QDBusConnection::sessionBus().interface()->servicePid(
                tryxRuntimeServiceName());
        if (!pidReply.isValid() ||
            static_cast<qint64>(pidReply.value()) != process.pid()) {
            return;
        }
        QThread::msleep(10);
    }
}

bool runtimeServiceIsRegistered() {
    return !runtimeServiceOwner().isEmpty();
}

RuntimeApiStatus probeRuntimeApi(const QString &service,
                                 QString *errorMessage,
                                 int dbusCallTimeoutMs) {
    QDBusInterface runtime(
        service, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QDBusConnection::sessionBus());
    runtime.setTimeout(qMax(1, dbusCallTimeoutMs));
    const QDBusReply<quint32> reply =
        runtime.call(QStringLiteral("GetRuntimeApiVersion"));
    if (!reply.isValid()) {
        if (errorMessage) {
            *errorMessage = reply.error().message();
        }
        return RuntimeApiStatus::Unavailable;
    }
    if (reply.value() != tryxRuntimeApiVersion()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The running TRYX runtime uses API %1, but this client requires API %2")
                                .arg(reply.value())
                                .arg(tryxRuntimeApiVersion());
        }
        return RuntimeApiStatus::VersionMismatch;
    }
    return RuntimeApiStatus::Compatible;
}

RuntimeApiStatus waitForRuntimeApi(
    const QString &service,
    const RuntimeBootstrapOptions &options,
    QString *errorMessage) {
    const int timeoutMs = qMax(1, options.startupTimeoutMs);
    QElapsedTimer deadline;
    deadline.start();
    QString lastError;
    do {
        const int remaining = qMax(
            1, timeoutMs -
                   static_cast<int>(deadline.elapsed()));
        const RuntimeApiStatus status = probeRuntimeApi(
            service, &lastError,
            qMin(qMax(1, options.dbusCallTimeoutMs), remaining));
        if (status != RuntimeApiStatus::Unavailable) {
            if (runtimeServiceOwner() != service) {
                lastError = QObject::tr(
                    "The TRYX runtime D-Bus owner changed while its API was being checked");
                break;
            }
            if (errorMessage) {
                *errorMessage = lastError;
            }
            return status;
        }
        if (deadline.elapsed() >= timeoutMs) {
            break;
        }
        QThread::msleep(25);
    } while (runtimeServiceOwner() == service);

    if (errorMessage) {
        *errorMessage = lastError.isEmpty()
            ? QObject::tr(
                  "The TRYX runtime D-Bus API was not ready before the startup deadline")
            : lastError;
    }
    return RuntimeApiStatus::Unavailable;
}

bool waitForRuntimeService(int timeoutMs,
                           QString *errorMessage) {
    if (runtimeServiceIsRegistered()) {
        return true;
    }
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("The user D-Bus session is unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QDBusServiceWatcher watcher(
        tryxRuntimeServiceName(), bus,
        QDBusServiceWatcher::WatchForRegistration);
    QObject::connect(
        &watcher, &QDBusServiceWatcher::serviceRegistered,
        &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout,
                     &loop, &QEventLoop::quit);
    deadline.start(qMax(1, timeoutMs));
    loop.exec();
    if (runtimeServiceIsRegistered()) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QObject::tr(
            "TRYX background runtime did not acquire its D-Bus name before the startup deadline");
    }
    return false;
}

bool controlRuntimeThroughSystemd(
    const QString &action,
    const RuntimeBootstrapOptions &options,
    bool *unitMissing,
    QString *errorMessage) {
    if (unitMissing) {
        *unitMissing = false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        options.systemctlProgram,
        {QStringLiteral("--user"), action,
         QStringLiteral("tryx-panorama.service")});
    if (!process.waitForStarted(
            qMin(kProcessStartTimeoutMs,
                 qMax(1, options.startupTimeoutMs)))) {
        if (unitMissing) {
            *unitMissing = true;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Failed to start systemctl: %1")
                                .arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(
            qMax(1, options.startupTimeoutMs))) {
        process.kill();
        process.waitForFinished(1000);
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl did not finish the TRYX runtime action before the deadline");
        }
        return false;
    }
    const QString output =
        QString::fromLocal8Bit(process.readAll()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0) {
        return true;
    }
    const bool missing =
        output.contains(QStringLiteral("not found"),
                        Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("not be found"),
                        Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("not loaded"),
                        Qt::CaseInsensitive);
    if (unitMissing) {
        *unitMissing = missing;
    }
    if (errorMessage) {
        *errorMessage = output.isEmpty()
            ? QObject::tr("systemctl failed with exit code %1")
                  .arg(process.exitCode())
            : output;
    }
    return false;
}

bool inspectRuntimeUnit(const RuntimeBootstrapOptions &options,
                        RuntimeUnitState *state,
                        QString *errorMessage) {
    if (!state) {
        return false;
    }
    QProcess process;
    process.start(
        options.systemctlProgram,
        {QStringLiteral("--user"), QStringLiteral("show"),
         QStringLiteral("tryx-panorama.service"),
         QStringLiteral("--property=LoadState"),
         QStringLiteral("--property=ActiveState"),
         QStringLiteral("--property=SubState"),
         QStringLiteral("--property=MainPID"),
         QStringLiteral("--property=Job"),
         QStringLiteral("--no-pager")});
    if (!process.waitForStarted(
            qMin(kProcessStartTimeoutMs,
                 qMax(1, options.startupTimeoutMs)))) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Failed to start systemctl: %1")
                                .arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(qMax(1, options.startupTimeoutMs))) {
        process.kill();
        process.waitForFinished(1000);
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl did not return the TRYX runtime state before the deadline");
        }
        return false;
    }

    const QString standardOutput =
        QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString standardError =
        QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = standardError.isEmpty()
                ? QObject::tr("systemctl failed with exit code %1")
                      .arg(process.exitCode())
                : standardError;
        }
        return false;
    }

    bool loadStateSeen = false;
    bool activeStateSeen = false;
    bool subStateSeen = false;
    bool mainPidSeen = false;
    bool jobSeen = false;
    bool mainPidValid = false;
    for (const QString &line :
         standardOutput.split('\n', Qt::SkipEmptyParts)) {
        const qsizetype separator = line.indexOf('=');
        if (separator < 0) {
            continue;
        }
        const QString key = line.left(separator);
        const QString value = line.mid(separator + 1).trimmed();
        if (key == QStringLiteral("LoadState")) {
            state->loadState = value;
            loadStateSeen = true;
        } else if (key == QStringLiteral("ActiveState")) {
            state->activeState = value;
            activeStateSeen = true;
        } else if (key == QStringLiteral("SubState")) {
            state->subState = value;
            subStateSeen = true;
        } else if (key == QStringLiteral("MainPID")) {
            state->mainPid = value.toULongLong(&mainPidValid);
            mainPidSeen = true;
        } else if (key == QStringLiteral("Job")) {
            state->job = value;
            jobSeen = true;
        }
    }
    if (!loadStateSeen || !activeStateSeen || !subStateSeen ||
        !mainPidSeen || !mainPidValid || !jobSeen) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl returned an incomplete TRYX runtime state");
        }
        return false;
    }
    return true;
}

bool runtimeUnitIsQuiescent(const RuntimeUnitState &state) {
    const bool inactive =
        state.activeState == QStringLiteral("inactive") ||
        state.activeState == QStringLiteral("failed");
    return inactive && state.mainPid == 0 && state.job.isEmpty();
}

QString verifiedExecutable(const QString &candidate) {
    const QFileInfo info(candidate);
    if (!info.exists() || !info.isFile() ||
        info.isSymLink() || !info.isExecutable()) {
        return {};
    }
    return info.canonicalFilePath();
}

DevelopmentRuntimeSelection selectDevelopmentRuntime(
    const RuntimeBootstrapOptions &options) {
    DevelopmentRuntimeSelection selection;
    QString candidate;
    if (!options.developmentRuntimeProgram.isEmpty()) {
        selection.requested = true;
        candidate = options.developmentRuntimeProgram;
    } else if (options.discoverDevelopmentRuntime) {
#if defined(TRYX_BUILD_QUICK_DIRECTORY)
        const QDir quickBinaryDirectory(
            QCoreApplication::applicationDirPath());
        const QDir configuredBuildDirectory(
            QStringLiteral(TRYX_BUILD_QUICK_DIRECTORY));
        if (!quickBinaryDirectory.canonicalPath().isEmpty() &&
            quickBinaryDirectory.canonicalPath() ==
                configuredBuildDirectory.canonicalPath()) {
            selection.requested = true;
            candidate = quickBinaryDirectory.absoluteFilePath(
                QStringLiteral(
                    "../runtime/tryx-panorama-runtime"));
        }
#endif
    }
    if (!selection.requested) {
        return selection;
    }
    selection.program = verifiedExecutable(candidate);
    if (selection.program.isEmpty()) {
        selection.error = QObject::tr(
            "The build-tree TRYX runtime is missing or is not an executable regular file: %1")
                              .arg(QDir::cleanPath(candidate));
    }
    return selection;
}

bool startRuntimeExecutable(
    const QString &executable,
    const QStringList &arguments,
    StartedRuntimeProcess *startedProcess,
    QString *errorMessage) {
    qint64 pid = -1;
    if (executable.isEmpty() || !QProcess::startDetached(
                                    executable, arguments, {}, &pid)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The TRYX runtime executable could not be started: %1")
                                .arg(executable);
        }
        return false;
    }
    if (startedProcess) {
        startedProcess->capture(pid, executable);
    }
    return true;
}

bool startFallbackRuntime(const RuntimeBootstrapOptions &options,
                          StartedRuntimeProcess *startedProcess,
                          QString *errorMessage) {
    QString executable;
    if (!options.installedRuntimeFallbackProgram.isEmpty()) {
        executable = verifiedExecutable(
            options.installedRuntimeFallbackProgram);
    } else {
        const QString installedRuntime =
            QStringLiteral(
                "/usr/lib/tryx-panorama-manager/tryx-panorama-runtime");
        executable = verifiedExecutable(installedRuntime);
        if (executable.isEmpty()) {
            executable = QStandardPaths::findExecutable(
                QStringLiteral("tryx-panorama-runtime"));
        }
    }
    if (!startRuntimeExecutable(
            executable,
            options.installedRuntimeFallbackArguments,
            startedProcess, nullptr)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The systemd unit is not installed and the development runtime could not be started");
        }
        return false;
    }
    return true;
}

}  // namespace

namespace {

bool runtimeDirectoryHierarchyIsSafe(const QString &runtimeDirectory) {
#if defined(Q_OS_LINUX)
    QString current = QDir::cleanPath(runtimeDirectory);
    bool runtimeLeaf = true;
    while (true) {
        const QByteArray encoded = QFile::encodeName(current);
        struct stat status {};
        if (::lstat(encoded.constData(), &status) != 0 ||
            !S_ISDIR(status.st_mode)) {
            return false;
        }
        if (runtimeLeaf) {
            if (status.st_uid != ::getuid() ||
                (status.st_mode & 0777) != S_IRWXU) {
                return false;
            }
            runtimeLeaf = false;
        } else {
            if (status.st_uid != 0 && status.st_uid != ::getuid()) {
                return false;
            }
            const bool broadlyWritable =
                (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
            if (broadlyWritable &&
                (status.st_mode & S_ISVTX) == 0) {
                return false;
            }
        }
        if (current == QStringLiteral("/")) {
            return true;
        }
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) {
            return false;
        }
        current = parent;
    }
#else
    Q_UNUSED(runtimeDirectory);
    return false;
#endif
}

bool pinnedRuntimeDirectoryStillMatches(
    const QString &path,
    dev_t expectedDevice,
    ino_t expectedInode) {
#if defined(Q_OS_LINUX)
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISDIR(status.st_mode) &&
           status.st_uid == ::getuid() &&
           (status.st_mode & 0777) == S_IRWXU &&
           status.st_dev == expectedDevice &&
           status.st_ino == expectedInode;
#else
    Q_UNUSED(path);
    Q_UNUSED(expectedDevice);
    Q_UNUSED(expectedInode);
    return false;
#endif
}

QString descriptorRelativePath(int directoryDescriptor,
                               const QByteArray &leafName) {
    return QStringLiteral("/proc/self/fd/%1/%2")
        .arg(directoryDescriptor)
        .arg(QString::fromLocal8Bit(leafName));
}

}  // namespace

struct SingleInstanceGuard::Private {
    ~Private() {
        lock.reset();
#if defined(Q_OS_LINUX)
        if (directoryDescriptor >= 0) {
            ::close(directoryDescriptor);
        }
#endif
    }

    int directoryDescriptor = -1;
    QString directoryPath;
    QByteArray socketLeafName;
    QString operationalSocketPath;
    dev_t directoryDevice = 0;
    ino_t directoryInode = 0;
    std::unique_ptr<QLockFile> lock;
    bool published = false;
    SocketIdentity publishedIdentity;
};

QString instanceSocketPath() {
    const QString configuredRuntimeDirectory =
        gRuntimeDirectoryOverrideSet
            ? gRuntimeDirectoryOverride
            : QStandardPaths::writableLocation(
                  QStandardPaths::RuntimeLocation);
    if (configuredRuntimeDirectory.isEmpty() ||
        !QDir::isAbsolutePath(configuredRuntimeDirectory)) {
        return {};
    }
    const QString runtimeDirectory =
        QDir::cleanPath(configuredRuntimeDirectory);
#if defined(Q_OS_LINUX)
    if (!runtimeDirectoryHierarchyIsSafe(runtimeDirectory)) {
        return {};
    }
    const QByteArray encodedDirectory = QFile::encodeName(runtimeDirectory);
    const int descriptor = ::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return {};
    }
    struct stat status {};
    const bool privateDirectory =
        ::fstat(descriptor, &status) == 0 &&
        S_ISDIR(status.st_mode) &&
        status.st_uid == ::getuid() &&
        (status.st_mode & 0777) == S_IRWXU;
    ::close(descriptor);
    if (!privateDirectory) {
        return {};
    }
#else
    return {};
#endif
    return QDir(runtimeDirectory).filePath(
        QStringLiteral("tryx-panorama-manager.instance"));
}

QByteArray instanceLaunchIntentPayload(InstanceLaunchIntent intent) {
    switch (intent) {
    case InstanceLaunchIntent::Manual:
        return QByteArrayLiteral("show");
    case InstanceLaunchIntent::Autostart:
        return QByteArrayLiteral("autostart");
    }
    return {};
}

std::optional<InstanceLaunchIntent> parseInstanceLaunchIntentPayload(
    const QByteArray &payload) {
    if (payload == QByteArrayLiteral("show")) {
        return InstanceLaunchIntent::Manual;
    }
    if (payload == QByteArrayLiteral("autostart")) {
        return InstanceLaunchIntent::Autostart;
    }
    return std::nullopt;
}

bool instanceLaunchIntentRequestsWindow(
    InstanceLaunchIntent intent) {
    switch (intent) {
    case InstanceLaunchIntent::Manual:
        return true;
    case InstanceLaunchIntent::Autostart:
        return false;
    }
    return false;
}

bool notifyRunningInstance(const QString &path,
                           InstanceLaunchIntent intent) {
    return notifyRunningInstanceWithResult(path, intent) ==
           InstanceNotificationResult::Delivered;
}

SingleInstanceGuard::SingleInstanceGuard(QString socketPath)
    : socketPath_(QDir::cleanPath(std::move(socketPath))),
      private_(std::make_unique<Private>()) {
#if defined(Q_OS_LINUX)
    if (socketPath_.isEmpty() ||
        !QDir::isAbsolutePath(socketPath_)) {
        return;
    }
    const QFileInfo socketInfo(socketPath_);
    private_->directoryPath = socketInfo.absolutePath();
    private_->socketLeafName =
        QFile::encodeName(socketInfo.fileName());
    if (private_->socketLeafName.isEmpty() ||
        private_->socketLeafName == QByteArrayLiteral(".") ||
        private_->socketLeafName == QByteArrayLiteral("..") ||
        !runtimeDirectoryHierarchyIsSafe(private_->directoryPath)) {
        return;
    }
    const QByteArray encodedDirectory =
        QFile::encodeName(private_->directoryPath);
    private_->directoryDescriptor = ::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat directoryStatus {};
    if (private_->directoryDescriptor < 0 ||
        ::fstat(
            private_->directoryDescriptor,
            &directoryStatus) != 0 ||
        !S_ISDIR(directoryStatus.st_mode) ||
        directoryStatus.st_uid != ::getuid() ||
        (directoryStatus.st_mode & 0777) != S_IRWXU) {
        return;
    }
    private_->directoryDevice = directoryStatus.st_dev;
    private_->directoryInode = directoryStatus.st_ino;
    private_->operationalSocketPath = descriptorRelativePath(
        private_->directoryDescriptor,
        private_->socketLeafName);
    private_->lock = std::make_unique<QLockFile>(
        private_->operationalSocketPath + QStringLiteral(".lock"));
    // This lock protects the socket for the whole GUI lifetime. Time-based
    // expiry could otherwise let a second process steal a healthy long-lived
    // owner; QLockFile still detects a crashed local owner by PID.
    private_->lock->setStaleLockTime(0);
    if (gAfterRuntimeDirectoryPinnedHook) {
        gAfterRuntimeDirectoryPinnedHook(private_->directoryPath);
    }
#endif
}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (private_ && private_->published &&
        private_->directoryDescriptor >= 0) {
        unlinkSocketIfMatchesAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            private_->publishedIdentity);
    }
}

SingleInstanceAcquireResult SingleInstanceGuard::acquire(
    QLocalServer *server,
    InstanceLaunchIntent intent,
    QString *errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!server || !private_ || !private_->lock ||
        private_->directoryDescriptor < 0 ||
        socketPath_.isEmpty() || server->isListening() ||
        private_->lock->isLocked() ||
        !pinnedRuntimeDirectoryStillMatches(
            private_->directoryPath,
            private_->directoryDevice,
            private_->directoryInode)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Invalid single-instance acquisition state");
        }
        return SingleInstanceAcquireResult::Failed;
    }

    // Preserve compatibility with an already running pre-lease build and
    // avoid touching any live socket before attempting stale recovery.
    const auto notificationFailedClosed =
        [errorMessage]() {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The existing single-instance endpoint did not acknowledge the launch intent");
            }
            return SingleInstanceAcquireResult::Failed;
        };
    qint64 previousLeasePid = 0;
    QString previousLeaseHost;
    QString previousLeaseApplication;
    bool observedPreviousLease = private_->lock->getLockInfo(
        &previousLeasePid,
        &previousLeaseHost,
        &previousLeaseApplication);
    if (gAfterPreviousLeaseProbeHook) {
        gAfterPreviousLeaseProbeHook(socketPath_);
    }
    InstanceNotificationResult notification =
        notifyRunningInstanceWithResult(
            private_->operationalSocketPath, intent);
    if (notification == InstanceNotificationResult::Delivered) {
        return SingleInstanceAcquireResult::NotifiedExisting;
    }
    if (notification ==
        InstanceNotificationResult::UnacknowledgedOwner) {
        return notificationFailedClosed();
    }

    bool lockAcquired = private_->lock->tryLock(0);
    if (!lockAcquired &&
        private_->lock->error() == QLockFile::LockFailedError) {
        observedPreviousLease =
            private_->lock->getLockInfo(
                &previousLeasePid,
                &previousLeaseHost,
                &previousLeaseApplication) ||
            observedPreviousLease;
        // The primary takes the lifetime lock before it removes a stale
        // socket and starts listening. A simultaneous launcher can therefore
        // observe a short interval where the lock exists but the socket does
        // not. Give that owner a bounded opportunity to finish startup.
        QElapsedTimer startupWait;
        startupWait.start();
        while (startupWait.elapsed() < kInstanceOwnerStartupWaitMs) {
            notification = notifyRunningInstanceWithResult(
                private_->operationalSocketPath, intent);
            if (notification ==
                InstanceNotificationResult::Delivered) {
                return SingleInstanceAcquireResult::NotifiedExisting;
            }
            if (notification ==
                InstanceNotificationResult::UnacknowledgedOwner) {
                return notificationFailedClosed();
            }
            // If the owner failed or crashed before listen(), take over under
            // the same lease instead of requiring a third launch to recover.
            observedPreviousLease =
                private_->lock->getLockInfo(
                    &previousLeasePid,
                    &previousLeaseHost,
                    &previousLeaseApplication) ||
                observedPreviousLease;
            if (private_->lock->tryLock(0)) {
                lockAcquired = true;
                break;
            }
            if (private_->lock->error() !=
                QLockFile::LockFailedError) {
                break;
            }
            QThread::msleep(kInstanceOwnerStartupRetryMs);
        }
    }
    if (!lockAcquired) {
        if (errorMessage) {
            switch (private_->lock->error()) {
            case QLockFile::LockFailedError:
                *errorMessage = QStringLiteral(
                    "The single-instance owner did not make its socket ready");
                break;
            case QLockFile::PermissionError:
                *errorMessage = QStringLiteral(
                    "Permission denied while acquiring the single-instance lock");
                break;
            case QLockFile::UnknownError:
            case QLockFile::NoError:
                *errorMessage = QStringLiteral(
                    "Could not acquire the single-instance lock");
                break;
            }
        }
        return SingleInstanceAcquireResult::Failed;
    }

    const auto unlockAndFail = [&]() {
        private_->lock->unlock();
        return SingleInstanceAcquireResult::Failed;
    };
    if (!pinnedRuntimeDirectoryStillMatches(
            private_->directoryPath,
            private_->directoryDevice,
            private_->directoryInode)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The runtime directory changed during single-instance acquisition");
        }
        return unlockAndFail();
    }

    SocketIdentity staleCandidate;
    const bool observedSocketBeforeFinalProbe =
        inspectSocketIdentityAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            &staleCandidate);

    // A pre-lease process may have appeared between the first probe and our
    // lock acquisition. Never remove its socket when it can still answer.
    notification = notifyRunningInstanceWithResult(
        private_->operationalSocketPath, intent);
    if (notification == InstanceNotificationResult::Delivered) {
        private_->lock->unlock();
        return SingleInstanceAcquireResult::NotifiedExisting;
    }
    if (notification ==
        InstanceNotificationResult::UnacknowledgedOwner) {
        private_->lock->unlock();
        return notificationFailedClosed();
    }

    NativeBoundSocket nativeSocket;
    const NativeSocketBindResult directBind = bindNativeUnixSocket(
        private_->operationalSocketPath,
        &nativeSocket, errorMessage);
    if (directBind == NativeSocketBindResult::Bound) {
        if (gAfterNativeSocketBoundHook) {
            gAfterNativeSocketBoundHook(socketPath_);
        }
        SocketIdentity publishedIdentity;
        mode_t publishedPermissions = 0;
        if (!inspectSocketIdentityAt(
                private_->directoryDescriptor,
                private_->socketLeafName,
                &publishedIdentity,
                &publishedPermissions) ||
            !sameSocketIdentity(
                nativeSocket.identity, publishedIdentity) ||
            publishedPermissions != (S_IRUSR | S_IWUSR) ||
            !pinnedRuntimeDirectoryStillMatches(
                private_->directoryPath,
                private_->directoryDevice,
                private_->directoryInode) ||
            !adoptNativeSocket(
                server, &nativeSocket, errorMessage)) {
            if (errorMessage && errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral(
                    "The single-instance socket changed before publication");
            }
            closeNativeBoundSocket(&nativeSocket);
            unlinkSocketIfMatchesAt(
                private_->directoryDescriptor,
                private_->socketLeafName,
                nativeSocket.identity);
            return unlockAndFail();
        }
        private_->published = true;
        private_->publishedIdentity = publishedIdentity;
        return SingleInstanceAcquireResult::Primary;
    }
    if (directBind == NativeSocketBindResult::Failed) {
        return unlockAndFail();
    }

    if (!observedPreviousLease ||
        !observedSocketBeforeFinalProbe) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Refusing to remove a single-instance socket without crashed-owner evidence");
        }
        return unlockAndFail();
    }

    if (gAfterLeaseAcquiredBeforeSocketCleanupHook) {
        gAfterLeaseAcquiredBeforeSocketCleanupHook(socketPath_);
    }
    SocketIdentity currentStaleCandidate;
    if (!inspectSocketIdentityAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            &currentStaleCandidate) ||
        !sameSocketIdentity(
            staleCandidate, currentStaleCandidate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The single-instance socket changed before stale recovery");
        }
        return unlockAndFail();
    }

    QByteArray replacementLeafName;
    NativeSocketBindResult replacementBind =
        NativeSocketBindResult::Failed;
    for (int attempt = 0; attempt < 128; ++attempt) {
        const quint64 sequence =
            gReplacementSocketSequence.fetch_add(
                1, std::memory_order_relaxed);
        replacementLeafName =
            QByteArrayLiteral(
                ".tryx-panorama-instance-replacement.") +
            QByteArray::number(
                static_cast<qulonglong>(::getpid())) +
            '.' + QByteArray::number(
                      static_cast<qulonglong>(sequence));
        replacementBind = bindNativeUnixSocket(
            descriptorRelativePath(
                private_->directoryDescriptor,
                replacementLeafName),
            &nativeSocket, errorMessage);
        if (replacementBind !=
            NativeSocketBindResult::AddressInUse) {
            break;
        }
    }
    if (replacementBind != NativeSocketBindResult::Bound) {
        return unlockAndFail();
    }
    if (!exchangeInReplacementSocket(
            private_->directoryDescriptor,
            private_->socketLeafName,
            replacementLeafName,
            staleCandidate,
            nativeSocket.identity,
            socketPath_, errorMessage)) {
        closeNativeBoundSocket(&nativeSocket);
        unlinkSocketIfMatchesAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            nativeSocket.identity);
        unlinkSocketIfMatchesAt(
            private_->directoryDescriptor,
            replacementLeafName,
            nativeSocket.identity);
        return unlockAndFail();
    }
    if (gAfterNativeSocketBoundHook) {
        gAfterNativeSocketBoundHook(socketPath_);
    }
    SocketIdentity publishedIdentity;
    mode_t publishedPermissions = 0;
    if (!inspectSocketIdentityAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            &publishedIdentity,
            &publishedPermissions) ||
        !sameSocketIdentity(
            nativeSocket.identity, publishedIdentity) ||
        publishedPermissions != (S_IRUSR | S_IWUSR) ||
        !pinnedRuntimeDirectoryStillMatches(
            private_->directoryPath,
            private_->directoryDevice,
            private_->directoryInode) ||
        !adoptNativeSocket(
            server, &nativeSocket, errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral(
                "The replacement single-instance socket changed before publication");
        }
        closeNativeBoundSocket(&nativeSocket);
        unlinkSocketIfMatchesAt(
            private_->directoryDescriptor,
            private_->socketLeafName,
            nativeSocket.identity);
        return unlockAndFail();
    }
    private_->published = true;
    private_->publishedIdentity = publishedIdentity;
    return SingleInstanceAcquireResult::Primary;
}

bool listenForSingleInstance(QLocalServer *server,
                             const QString &path,
                             QString *errorMessage) {
    if (!server || server->isListening()) {
        return false;
    }
    NativeBoundSocket nativeSocket;
    const NativeSocketBindResult bindResult = bindNativeUnixSocket(
        path, &nativeSocket, errorMessage);
    if (bindResult != NativeSocketBindResult::Bound) {
        if (errorMessage) {
            if (bindResult == NativeSocketBindResult::AddressInUse) {
                *errorMessage = QStringLiteral(
                    "The single-instance socket is already in use");
            } else if (errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral(
                    "Could not prepare the single-instance socket");
            }
        }
        return false;
    }
    const SocketIdentity publishedIdentity = nativeSocket.identity;
    if (!adoptNativeSocket(server, &nativeSocket, errorMessage)) {
        SocketIdentity currentIdentity;
        if (inspectSocketIdentity(path, &currentIdentity) &&
            sameSocketIdentity(
                publishedIdentity, currentIdentity)) {
            QFile::remove(path);
        }
        return false;
    }
    QObject::connect(
        server, &QObject::destroyed,
        [path, publishedIdentity]() {
            SocketIdentity currentIdentity;
            if (inspectSocketIdentity(path, &currentIdentity) &&
                sameSocketIdentity(
                    publishedIdentity, currentIdentity)) {
                QFile::remove(path);
            }
        });
    return true;
}

namespace testing {

void setRuntimeDirectoryOverride(const QString &directory) {
    gRuntimeDirectoryOverride = directory;
    gRuntimeDirectoryOverrideSet = true;
}

void clearRuntimeDirectoryOverride() {
    gRuntimeDirectoryOverride.clear();
    gRuntimeDirectoryOverrideSet = false;
}

void setAfterLeaseAcquiredBeforeSocketCleanupHook(
    AfterLeaseAcquiredBeforeSocketCleanupHook hook) {
    gAfterLeaseAcquiredBeforeSocketCleanupHook = hook;
}

void clearAfterLeaseAcquiredBeforeSocketCleanupHook() {
    gAfterLeaseAcquiredBeforeSocketCleanupHook = nullptr;
}

void setAfterPreviousLeaseProbeHook(
    AfterPreviousLeaseProbeHook hook) {
    gAfterPreviousLeaseProbeHook = hook;
}

void clearAfterPreviousLeaseProbeHook() {
    gAfterPreviousLeaseProbeHook = nullptr;
}

void setBeforeStaleSocketExchangeHook(
    BeforeStaleSocketExchangeHook hook) {
    gBeforeStaleSocketExchangeHook = hook;
}

void clearBeforeStaleSocketExchangeHook() {
    gBeforeStaleSocketExchangeHook = nullptr;
}

void setAfterStaleSocketExchangeHook(
    AfterStaleSocketExchangeHook hook) {
    gAfterStaleSocketExchangeHook = hook;
}

void clearAfterStaleSocketExchangeHook() {
    gAfterStaleSocketExchangeHook = nullptr;
}

void setAfterNativeSocketBoundHook(
    AfterNativeSocketBoundHook hook) {
    gAfterNativeSocketBoundHook = hook;
}

void clearAfterNativeSocketBoundHook() {
    gAfterNativeSocketBoundHook = nullptr;
}

void setAfterRuntimeDirectoryPinnedHook(
    AfterRuntimeDirectoryPinnedHook hook) {
    gAfterRuntimeDirectoryPinnedHook = hook;
}

void clearAfterRuntimeDirectoryPinnedHook() {
    gAfterRuntimeDirectoryPinnedHook = nullptr;
}

}  // namespace testing

void drainPendingInstanceLaunchConnections(
    QLocalServer *server,
    QObject *context,
    InstanceLaunchIntentHandler handler) {
    if (!server || !context) {
        return;
    }
    while (QLocalSocket *connection =
               server->nextPendingConnection()) {
        new PendingInstanceLaunchReader(
            connection, handler, context);
    }
}

bool ensureRuntimeService(QString *errorMessage) {
    return ensureRuntimeService(
        errorMessage, RuntimeBootstrapOptions{});
}

bool ensureRuntimeService(
    QString *errorMessage,
    const RuntimeBootstrapOptions &options) {
    registerTryxRuntimeMetaTypes();
    const DevelopmentRuntimeSelection developmentRuntime =
        selectDevelopmentRuntime(options);
    const QString existingOwner = runtimeServiceOwner();
    if (!existingOwner.isEmpty()) {
        QString compatibilityError;
        const RuntimeApiStatus apiStatus = waitForRuntimeApi(
            existingOwner, options, &compatibilityError);
        if (apiStatus == RuntimeApiStatus::Compatible) {
            return true;
        }
        if (apiStatus == RuntimeApiStatus::Unavailable) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The running TRYX runtime API could not be verified; it was not restarted: %1")
                                    .arg(compatibilityError);
            }
            return false;
        }

        // API 6 has no atomic quiesce-and-replace operation. Never stop it
        // automatically after a separate state probe because another client
        // could start hardware work between the probe and the stop action.
        if (developmentRuntime.requested) {
            if (errorMessage) {
                *errorMessage = developmentRuntime.program.isEmpty()
                    ? developmentRuntime.error
                    : QObject::tr(
                          "An incompatible TRYX runtime is already running. Stop the existing runtime (for the installed service: systemctl --user stop tryx-panorama.service), then reopen this build: %1")
                          .arg(compatibilityError);
            }
            return false;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "An incompatible installed TRYX runtime is already running. Finish or cancel any active operation, restart it with systemctl --user restart tryx-panorama.service, then reopen the GUI: %1")
                                .arg(compatibilityError);
        }
        return false;
    }

    StartedRuntimeProcess startedDirectRuntime;
    bool directRuntimeStarted = false;

    // A source build must use its sibling runtime. Starting systemd first
    // could resurrect an older installed runtime instead. Conversely, do not
    // start the sibling while an installed process or queued systemd job is
    // still racing to acquire D-Bus.
    if (developmentRuntime.requested) {
        if (developmentRuntime.program.isEmpty()) {
            if (errorMessage) {
                *errorMessage = developmentRuntime.error;
            }
            return false;
        }
        RuntimeUnitState unitState;
        QString unitStateError;
        if (!inspectRuntimeUnit(options, &unitState, &unitStateError)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime state could not be verified; the build-tree runtime was not started: %1")
                                    .arg(unitStateError);
            }
            return false;
        }
        if (!runtimeUnitIsQuiescent(unitState)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime is still running, starting, or stopping (%1/%2, PID %3, job %4), so the build-tree runtime was not started. Stop it with systemctl --user stop tryx-panorama.service, then reopen this build.")
                                    .arg(unitState.activeState,
                                         unitState.subState)
                                    .arg(unitState.mainPid)
                                    .arg(unitState.job.isEmpty()
                                             ? QStringLiteral("-")
                                             : unitState.job);
            }
            return false;
        }
        const QString appearedOwner = runtimeServiceOwner();
        if (!appearedOwner.isEmpty()) {
            QString compatibilityError;
            if (waitForRuntimeApi(
                    appearedOwner, options,
                    &compatibilityError) ==
                RuntimeApiStatus::Compatible) {
                return true;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "A TRYX runtime appeared while the installed unit state was being checked; the build-tree runtime was not started. Reopen this build: %1")
                                    .arg(compatibilityError);
            }
            return false;
        }
        if (!startRuntimeExecutable(
                developmentRuntime.program,
                options.developmentRuntimeArguments,
                &startedDirectRuntime,
                errorMessage)) {
            return false;
        }
        directRuntimeStarted = true;
    } else {
        bool unitMissing = false;
        QString systemdError;
        const bool systemdStarted = controlRuntimeThroughSystemd(
            QStringLiteral("start"), options,
            &unitMissing, &systemdError);
        if (!systemdStarted && !unitMissing) {
            if (errorMessage) {
                *errorMessage = systemdError;
            }
            return false;
        }
        if (!systemdStarted) {
            if (!options.allowInstalledRuntimeFallback) {
                if (errorMessage) {
                    *errorMessage = systemdError;
                }
                return false;
            }
            if (!startFallbackRuntime(
                    options, &startedDirectRuntime,
                    errorMessage)) {
                return false;
            }
            directRuntimeStarted = true;
        }
    }

    QString waitError;
    if (!waitForRuntimeService(
            options.startupTimeoutMs, &waitError)) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = waitError;
        }
        return false;
    }
    const QString startedOwner = runtimeServiceOwner();
    if (startedOwner.isEmpty()) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The TRYX runtime exited before its D-Bus API became ready");
        }
        return false;
    }
    QString compatibilityError;
    const RuntimeApiStatus startedApiStatus = waitForRuntimeApi(
        startedOwner, options, &compatibilityError);
    if (startedApiStatus != RuntimeApiStatus::Compatible) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = startedApiStatus ==
                    RuntimeApiStatus::VersionMismatch
                ? QObject::tr(
                      "The TRYX runtime started, but its API is incompatible: %1")
                      .arg(compatibilityError)
                : QObject::tr(
                      "The TRYX runtime started, but its D-Bus API did not become available: %1")
                      .arg(compatibilityError);
        }
        return false;
    }
    return true;
}

}  // namespace quickbootstrap
