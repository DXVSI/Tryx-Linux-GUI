#include "tryxclirunner.h"

#include "runtimecontract.h"
#include "runtimedowngradestore.h"
#include "supportbundle.h"
#include "supportsnapshot.h"

#include <QDateTime>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUrl>

#include <limits>

namespace tryx::cli {

namespace {

constexpr auto kDbusService = "org.freedesktop.DBus";
constexpr auto kDbusPath = "/org/freedesktop/DBus";
constexpr auto kDbusInterface = "org.freedesktop.DBus";
constexpr auto kGetNameOwner = "GetNameOwner";
constexpr auto kUnknownMethodError =
    "org.freedesktop.DBus.Error.UnknownMethod";
constexpr auto kNameHasNoOwnerError =
    "org.freedesktop.DBus.Error.NameHasNoOwner";
constexpr auto kServiceUnknownError =
    "org.freedesktop.DBus.Error.ServiceUnknown";
constexpr auto kDowngradeV10PreparationMethod =
    "PrepareRuntimeDowngradeV10";
constexpr auto kDowngradeStateEmpty = "Empty";
constexpr auto kDowngradeStateFullFrame = "FullFrame";
constexpr int kOwnerAbsenceStabilityMilliseconds = 100;
constexpr int kOwnerFencePollMilliseconds = 20;

enum class Command {
    Status,
    Capabilities,
    Operations,
    SupportReport,
    PrepareDowngradeV10,
    AbortDowngradeV10,
};

enum class Failure {
    None,
    SessionBusUnavailable,
    RuntimeUnavailable,
    IncompatibleRuntime,
    Unsupported,
    Timeout,
    OwnerChanged,
    RuntimeCallFailed,
    InvalidReply,
    UnsafeDestination,
    AlreadyExists,
    ExportIo,
    DowngradeBlocked,
    Internal,
};

struct Invocation {
    Command command = Command::Status;
    bool json = false;
    QString outputDirectory;
};

struct OwnerResult {
    Failure failure = Failure::Internal;
    QString owner;

    bool ok() const {
        return failure == Failure::None;
    }
};

struct RuntimeCallResult {
    Failure failure = Failure::Internal;
    QDBusMessage reply;
    bool exactUnknownMethod = false;

    bool ok() const {
        return failure == Failure::None;
    }
};

struct HandshakeResult {
    Failure failure = Failure::Internal;
    QString owner;
    quint32 runtimeApiVersion = 0;
    bool legacyRuntimeCapabilities = false;
    QStringList runtimeCapabilities;

    bool ok() const {
        return failure == Failure::None;
    }
};

struct SnapshotResult {
    Failure failure = Failure::Internal;
    QJsonObject root;

    bool ok() const {
        return failure == Failure::None;
    }
};

class Deadline final {
public:
    explicit Deadline(int totalMilliseconds)
        : totalMilliseconds_(totalMilliseconds) {
        timer_.start();
    }

    int remainingMilliseconds() const {
        if (totalMilliseconds_ <= 0) {
            return 0;
        }
        const qint64 remaining =
            static_cast<qint64>(totalMilliseconds_) - timer_.elapsed();
        return remaining > 0
            ? static_cast<int>(qMin<qint64>(
                  remaining,
                  static_cast<qint64>(
                      std::numeric_limits<int>::max())))
            : 0;
    }

private:
    int totalMilliseconds_ = 0;
    QElapsedTimer timer_;
};

QString commandName(Command command) {
    switch (command) {
    case Command::Status:
        return QStringLiteral("status");
    case Command::Capabilities:
        return QStringLiteral("capabilities");
    case Command::Operations:
        return QStringLiteral("operations");
    case Command::SupportReport:
        return QStringLiteral("support-report");
    case Command::PrepareDowngradeV10:
        return QStringLiteral("prepare-downgrade-v10");
    case Command::AbortDowngradeV10:
        return QStringLiteral("abort-downgrade-v10");
    }
    return {};
}

bool parseInvocation(
    const QStringList &arguments,
    Invocation *invocation) {
    if (!invocation || arguments.isEmpty()) {
        return false;
    }

    qsizetype commandIndex = 0;
    if (arguments.first() == QStringLiteral("--json")) {
        invocation->json = true;
        commandIndex = 1;
    }
    if (commandIndex >= arguments.size()) {
        return false;
    }

    const QString &command = arguments.at(commandIndex);
    const qsizetype remaining =
        arguments.size() - commandIndex;
    if (command == QStringLiteral("status") && remaining == 1) {
        invocation->command = Command::Status;
        return true;
    }
    if (command == QStringLiteral("capabilities") &&
        remaining == 1) {
        invocation->command = Command::Capabilities;
        return true;
    }
    if (command == QStringLiteral("operations") &&
        remaining == 1) {
        invocation->command = Command::Operations;
        return true;
    }
    if (command == QStringLiteral("prepare-downgrade-v10") &&
        remaining == 1) {
        invocation->command = Command::PrepareDowngradeV10;
        return true;
    }
    if (command == QStringLiteral("abort-downgrade-v10") &&
        remaining == 1) {
        invocation->command = Command::AbortDowngradeV10;
        return true;
    }
    if (command == QStringLiteral("support-report") &&
        remaining == 3 &&
        arguments.at(commandIndex + 1) ==
            QStringLiteral("--output-dir") &&
        !arguments.at(commandIndex + 2).isEmpty()) {
        invocation->command = Command::SupportReport;
        invocation->outputDirectory =
            arguments.at(commandIndex + 2);
        return true;
    }
    return false;
}

int exitCode(Failure failure) {
    switch (failure) {
    case Failure::SessionBusUnavailable:
        return 3;
    case Failure::RuntimeUnavailable:
        return 4;
    case Failure::IncompatibleRuntime:
        return 5;
    case Failure::Unsupported:
        return 6;
    case Failure::Timeout:
        return 7;
    case Failure::OwnerChanged:
        return 8;
    case Failure::RuntimeCallFailed:
        return 9;
    case Failure::InvalidReply:
        return 10;
    case Failure::UnsafeDestination:
        return 11;
    case Failure::AlreadyExists:
        return 12;
    case Failure::ExportIo:
        return 13;
    case Failure::DowngradeBlocked:
        return 14;
    case Failure::None:
    case Failure::Internal:
        return 70;
    }
    return 70;
}

QString errorCode(Failure failure) {
    switch (failure) {
    case Failure::SessionBusUnavailable:
        return QStringLiteral("SESSION_BUS_UNAVAILABLE");
    case Failure::RuntimeUnavailable:
        return QStringLiteral("RUNTIME_UNAVAILABLE");
    case Failure::IncompatibleRuntime:
        return QStringLiteral("INCOMPATIBLE_RUNTIME");
    case Failure::Unsupported:
        return QStringLiteral("UNSUPPORTED");
    case Failure::Timeout:
        return QStringLiteral("TIMEOUT");
    case Failure::OwnerChanged:
        return QStringLiteral("OWNER_CHANGED");
    case Failure::RuntimeCallFailed:
        return QStringLiteral("RUNTIME_CALL_FAILED");
    case Failure::InvalidReply:
        return QStringLiteral("INVALID_REPLY");
    case Failure::UnsafeDestination:
        return QStringLiteral("UNSAFE_DESTINATION");
    case Failure::AlreadyExists:
        return QStringLiteral("ALREADY_EXISTS");
    case Failure::ExportIo:
        return QStringLiteral("EXPORT_IO");
    case Failure::DowngradeBlocked:
        return QStringLiteral("DOWNGRADE_BLOCKED");
    case Failure::None:
    case Failure::Internal:
        return QStringLiteral("INTERNAL_ERROR");
    }
    return QStringLiteral("INTERNAL_ERROR");
}

QByteArray humanError(Failure failure) {
    switch (failure) {
    case Failure::SessionBusUnavailable:
        return QByteArrayLiteral(
            "User-session D-Bus is unavailable\n");
    case Failure::RuntimeUnavailable:
        return QByteArrayLiteral(
            "TRYX runtime is not running\n");
    case Failure::IncompatibleRuntime:
        return QByteArrayLiteral(
            "The TRYX runtime API is incompatible\n");
    case Failure::Unsupported:
        return QByteArrayLiteral(
            "The command is not supported by this runtime\n");
    case Failure::Timeout:
        return QByteArrayLiteral(
            "The TRYX runtime request timed out\n");
    case Failure::OwnerChanged:
        return QByteArrayLiteral(
            "The TRYX runtime changed during the request\n");
    case Failure::RuntimeCallFailed:
        return QByteArrayLiteral(
            "The TRYX runtime request failed\n");
    case Failure::InvalidReply:
        return QByteArrayLiteral(
            "The TRYX runtime returned an invalid reply\n");
    case Failure::UnsafeDestination:
        return QByteArrayLiteral(
            "The output directory is unsafe\n");
    case Failure::AlreadyExists:
        return QByteArrayLiteral(
            "The support report already exists\n");
    case Failure::ExportIo:
        return QByteArrayLiteral(
            "The support report could not be written\n");
    case Failure::DowngradeBlocked:
        return QByteArrayLiteral(
            "The TRYX runtime cannot prepare this downgrade safely\n");
    case Failure::None:
    case Failure::Internal:
        return QByteArrayLiteral(
            "An internal CLI error occurred\n");
    }
    return QByteArrayLiteral("An internal CLI error occurred\n");
}

QByteArray jsonEnvelope(
    const QString &command,
    bool ok,
    const QJsonObject &data,
    const QString &failureCode) {
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("command"), command);
    root.insert(QStringLiteral("ok"), ok);
    if (ok) {
        root.insert(QStringLiteral("data"), data);
        root.insert(
            QStringLiteral("error"),
            QJsonValue(QJsonValue::Null));
    } else {
        root.insert(
            QStringLiteral("data"),
            QJsonValue(QJsonValue::Null));
        root.insert(
            QStringLiteral("error"),
            QJsonObject{
                {QStringLiteral("code"), failureCode},
            });
    }
    QByteArray output =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    output.append('\n');
    return output;
}

RunResult failureResult(
    const Invocation &invocation,
    Failure failure) {
    RunResult result;
    result.exitCode = exitCode(failure);
    if (invocation.json) {
        result.standardOutput = jsonEnvelope(
            commandName(invocation.command), false, {},
            errorCode(failure));
    } else {
        result.standardError = humanError(failure);
    }
    return result;
}

RunResult successResult(
    const Invocation &invocation,
    const QJsonObject &data,
    const QByteArray &humanOutput) {
    RunResult result;
    result.exitCode = 0;
    result.standardOutput = invocation.json
        ? jsonEnvelope(
              commandName(invocation.command), true, data, {})
        : humanOutput;
    return result;
}

Failure writeFailure(
    tryx::support_bundle::WriteStatus status) {
    switch (status) {
    case tryx::support_bundle::WriteStatus::InvalidInput:
    case tryx::support_bundle::WriteStatus::UnsafeDestination:
        return Failure::UnsafeDestination;
    case tryx::support_bundle::WriteStatus::AlreadyExists:
        return Failure::AlreadyExists;
    case tryx::support_bundle::WriteStatus::IoError:
        return Failure::ExportIo;
    case tryx::support_bundle::WriteStatus::Success:
    case tryx::support_bundle::WriteStatus::PublishRejected:
        return Failure::Internal;
    }
    return Failure::Internal;
}

bool isTimeoutError(const QDBusMessage &reply) {
    if (reply.type() != QDBusMessage::ErrorMessage) {
        return false;
    }
    const QDBusError::ErrorType type = QDBusError(reply).type();
    return type == QDBusError::NoReply ||
        type == QDBusError::Timeout ||
        type == QDBusError::TimedOut;
}

Failure callError(const QDBusConnection &bus,
                  const QDBusMessage &reply) {
    if (!bus.isConnected()) {
        return Failure::SessionBusUnavailable;
    }
    return isTimeoutError(reply)
        ? Failure::Timeout
        : Failure::RuntimeCallFailed;
}

QDBusMessage boundedCall(
    const QDBusConnection &bus,
    QDBusMessage request,
    const Deadline &deadline,
    Failure *failure) {
    const int remaining = deadline.remainingMilliseconds();
    if (remaining <= 0) {
        *failure = Failure::Timeout;
        return {};
    }
    request.setAutoStartService(false);
    request.setInteractiveAuthorizationAllowed(false);
    const QDBusMessage reply =
        bus.call(request, QDBus::Block, remaining);
    *failure = Failure::None;
    return reply;
}

QDBusMessage makeRuntimeMethodCall(
    const QString &owner,
    const QString &method) {
    QDBusMessage request = QDBusMessage::createMethodCall(
        owner,
        tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        method);
    request.setAutoStartService(false);
    request.setInteractiveAuthorizationAllowed(false);
    return request;
}

OwnerResult resolveNameOwner(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &name) {
    if (!bus.isConnected()) {
        return {Failure::SessionBusUnavailable, {}};
    }

    QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(kDbusService),
        QString::fromLatin1(kDbusPath),
        QString::fromLatin1(kDbusInterface),
        QString::fromLatin1(kGetNameOwner));
    request.setArguments({name});

    Failure failure = Failure::Internal;
    const QDBusMessage reply =
        boundedCall(bus, request, deadline, &failure);
    if (failure != Failure::None) {
        return {failure, {}};
    }
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (reply.errorName() ==
                QString::fromLatin1(kNameHasNoOwnerError) ||
            reply.errorName() ==
                QString::fromLatin1(kServiceUnknownError)) {
            return {Failure::RuntimeUnavailable, {}};
        }
        return {callError(bus, reply), {}};
    }
    if (reply.type() != QDBusMessage::ReplyMessage ||
        reply.signature() != QStringLiteral("s") ||
        reply.arguments().size() != 1) {
        return {Failure::InvalidReply, {}};
    }
    const QDBusReply<QString> decoded(reply);
    const QString owner = decoded.value();
    if (!decoded.isValid() || !owner.startsWith(QLatin1Char(':')) ||
        owner.size() < 3) {
        return {Failure::InvalidReply, {}};
    }
    return {Failure::None, owner};
}

OwnerResult resolveOwner(
    const QDBusConnection &bus,
    const Deadline &deadline) {
    return resolveNameOwner(
        bus, deadline, tryxRuntimeServiceName());
}

Failure fenceOwner(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &expectedOwner) {
    const OwnerResult current = resolveOwner(bus, deadline);
    if (!current.ok()) {
        return current.failure == Failure::RuntimeUnavailable
            ? Failure::OwnerChanged
            : current.failure;
    }
    return current.owner == expectedOwner
        ? Failure::None
        : Failure::OwnerChanged;
}

RuntimeCallResult callRuntime(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &owner,
    const QString &method,
    const QString &expectedSignature,
    bool allowExactUnknownMethod = false) {
    QDBusMessage request = makeRuntimeMethodCall(owner, method);

    Failure failure = Failure::Internal;
    const QDBusMessage reply =
        boundedCall(bus, request, deadline, &failure);
    if (failure != Failure::None) {
        return {failure, {}, false};
    }

    const Failure fence = fenceOwner(bus, deadline, owner);
    if (fence != Failure::None) {
        return {fence, {}, false};
    }

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (allowExactUnknownMethod &&
            reply.errorName() ==
                QString::fromLatin1(kUnknownMethodError)) {
            return {Failure::None, {}, true};
        }
        return {callError(bus, reply), {}, false};
    }
    if (reply.type() != QDBusMessage::ReplyMessage ||
        reply.signature() != expectedSignature ||
        reply.arguments().size() != 1) {
        return {Failure::InvalidReply, {}, false};
    }
    return {Failure::None, reply, false};
}

Failure waitForOwnerExit(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &expectedOwner) {
    QElapsedTimer absenceTimer;

    while (deadline.remainingMilliseconds() > 0) {
        const OwnerResult exactOwner = resolveNameOwner(
            bus, deadline, expectedOwner);
        if (!exactOwner.ok() &&
            exactOwner.failure != Failure::RuntimeUnavailable) {
            return exactOwner.failure;
        }
        if (exactOwner.ok() &&
            exactOwner.owner != expectedOwner) {
            return Failure::OwnerChanged;
        }

        const OwnerResult advertisedOwner =
            resolveOwner(bus, deadline);
        if (!advertisedOwner.ok() &&
            advertisedOwner.failure !=
                Failure::RuntimeUnavailable) {
            return advertisedOwner.failure;
        }
        if (advertisedOwner.ok() &&
            advertisedOwner.owner != expectedOwner) {
            return Failure::OwnerChanged;
        }

        const bool exactOwnerExited =
            exactOwner.failure == Failure::RuntimeUnavailable;
        const bool serviceNameReleased =
            advertisedOwner.failure == Failure::RuntimeUnavailable;
        if (exactOwnerExited && serviceNameReleased) {
            if (!absenceTimer.isValid()) {
                absenceTimer.start();
            }
            if (absenceTimer.elapsed() >=
                kOwnerAbsenceStabilityMilliseconds) {
                return Failure::None;
            }
        } else {
            absenceTimer.invalidate();
        }

        const int remaining = deadline.remainingMilliseconds();
        if (remaining <= 0) {
            break;
        }
        QThread::msleep(static_cast<unsigned long>(qMin(
            remaining, kOwnerFencePollMilliseconds)));
    }
    return Failure::Timeout;
}

QStringList canonicalIntersection(
    const QStringList &canonical,
    const QStringList &filtered) {
    QStringList ordered;
    ordered.reserve(canonical.size());
    for (const QString &capability : canonical) {
        if (filtered.contains(capability)) {
            ordered.append(capability);
        }
    }
    return ordered;
}

QStringList deviceCapabilityAllowlist() {
    return {
        tryxDeviceMediaUploadV1Token(),
        tryxDeviceMediaCatalogV1Token(),
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceOverlayMetricsV1Token(),
        tryxDeviceFirmwareFlashV1Token(),
    };
}

HandshakeResult performHandshake(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &knownOwner = {}) {
    OwnerResult owner;
    if (knownOwner.isEmpty()) {
        owner = resolveOwner(bus, deadline);
        if (!owner.ok()) {
            return {owner.failure, {}, 0, false, {}};
        }
    } else {
        owner = {Failure::None, knownOwner};
    }

    const RuntimeCallResult apiCall = callRuntime(
        bus, deadline, owner.owner,
        QStringLiteral("GetRuntimeApiVersion"),
        QStringLiteral("u"));
    if (!apiCall.ok()) {
        return {apiCall.failure, {}, 0, false, {}};
    }
    const QDBusReply<quint32> apiReply(apiCall.reply);
    if (!apiReply.isValid()) {
        return {Failure::InvalidReply, {}, 0, false, {}};
    }
    const quint32 apiVersion = apiReply.value();
    if (apiVersion != tryxRuntimeApiVersion()) {
        return {Failure::IncompatibleRuntime, {}, 0, false, {}};
    }

    const RuntimeCallResult capabilityCall = callRuntime(
        bus, deadline, owner.owner,
        QStringLiteral("GetRuntimeCapabilities"),
        QStringLiteral("as"), true);
    if (!capabilityCall.ok()) {
        return {capabilityCall.failure, {}, 0, false, {}};
    }
    if (capabilityCall.exactUnknownMethod) {
        return {
            Failure::None,
            owner.owner,
            apiVersion,
            true,
            {},
        };
    }

    const QDBusReply<QStringList> capabilityReply(
        capabilityCall.reply);
    if (!capabilityReply.isValid()) {
        return {Failure::InvalidReply, {}, 0, false, {}};
    }
    const QStringList filtered = tryxFilterRuntimeCapabilities(
        capabilityReply.value());
    return {
        Failure::None,
        owner.owner,
        apiVersion,
        false,
        canonicalIntersection(
            tryxRuntimeCapabilities(), filtered),
    };
}

SnapshotResult collectSupportSnapshot(
    const QDBusConnection &bus,
    const Deadline &deadline,
    const HandshakeResult &handshake) {
    if (handshake.legacyRuntimeCapabilities ||
        !handshake.runtimeCapabilities.contains(
            tryxRuntimeSupportSnapshotV1Token())) {
        return {Failure::Unsupported, {}};
    }
    const RuntimeCallResult snapshotCall = callRuntime(
        bus, deadline, handshake.owner,
        QStringLiteral("GetSupportSnapshotV1"),
        QStringLiteral("s"));
    if (!snapshotCall.ok()) {
        return {snapshotCall.failure, {}};
    }
    const QDBusReply<QString> snapshotReply(snapshotCall.reply);
    if (!snapshotReply.isValid() ||
        !tryx::supportSnapshotV1IsValid(snapshotReply.value())) {
        return {Failure::InvalidReply, {}};
    }
    const QJsonDocument document = QJsonDocument::fromJson(
        snapshotReply.value().toUtf8());
    if (!document.isObject()) {
        return {Failure::Internal, {}};
    }
    return {Failure::None, document.object()};
}

QJsonObject statusData(const QJsonObject &snapshot) {
    return {
        {QStringLiteral("generated_at_utc"),
         snapshot.value(QStringLiteral("generated_at_utc"))},
        {QStringLiteral("runtime"),
         snapshot.value(QStringLiteral("runtime"))},
        {QStringLiteral("device"),
         snapshot.value(QStringLiteral("device"))},
        {QStringLiteral("recovery"),
         snapshot.value(QStringLiteral("recovery"))},
        {QStringLiteral("counts"),
         snapshot.value(QStringLiteral("counts"))},
    };
}

QByteArray humanStatus(const QJsonObject &data) {
    const QJsonObject runtime =
        data.value(QStringLiteral("runtime")).toObject();
    const QJsonObject device =
        data.value(QStringLiteral("device")).toObject();
    const QJsonObject counts =
        data.value(QStringLiteral("counts")).toObject();
    return QStringLiteral(
               "Runtime: %1 (API %2)\n"
               "Device: %3 [%4]\n"
               "Connected: %5\n"
               "Operations: %6\n"
               "Generated: %7\n")
        .arg(
            runtime.value(QStringLiteral("version")).toString(),
            QString::number(
                runtime.value(QStringLiteral("api_version"))
                    .toInteger()),
            device.value(QStringLiteral("model")).toString(),
            device.value(QStringLiteral("product_id")).toString(),
            device.value(QStringLiteral("connected")).toBool()
                ? QStringLiteral("yes")
                : QStringLiteral("no"),
            QString::number(
                counts.value(QStringLiteral("operation_count"))
                    .toInteger()),
            data.value(QStringLiteral("generated_at_utc"))
                .toString())
        .toUtf8();
}

QJsonObject operationsData(const QJsonObject &snapshot) {
    const QJsonArray items =
        snapshot.value(QStringLiteral("operations")).toArray();
    const qint64 operationCount = snapshot
        .value(QStringLiteral("counts"))
        .toObject()
        .value(QStringLiteral("operation_count"))
        .toInteger();
    return {
        {QStringLiteral("generated_at_utc"),
         snapshot.value(QStringLiteral("generated_at_utc"))},
        {QStringLiteral("operation_count"), operationCount},
        {QStringLiteral("returned_count"), items.size()},
        {QStringLiteral("truncated"),
         operationCount > items.size()},
        {QStringLiteral("items"), items},
    };
}

QByteArray humanOperations(const QJsonObject &data) {
    QString output = QStringLiteral(
                         "Operations: %1 total, %2 returned%3\n")
        .arg(
            QString::number(
                data.value(QStringLiteral("operation_count"))
                    .toInteger()),
            QString::number(
                data.value(QStringLiteral("returned_count"))
                    .toInteger()),
            data.value(QStringLiteral("truncated")).toBool()
                ? QStringLiteral(" (truncated)")
                : QString());
    for (const QJsonValue &value :
         data.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = value.toObject();
        output.append(QStringLiteral("%1 %2 %3\n").arg(
            item.value(QStringLiteral("kind")).toString(),
            item.value(QStringLiteral("state")).toString(),
            item.value(QStringLiteral("stage")).toString()));
    }
    return output.toUtf8();
}

QJsonObject capabilitiesData(
    const HandshakeResult &handshake,
    const QString &deviceStatus,
    const QStringList &deviceCapabilities) {
    return {
        {QStringLiteral("runtime_api_version"),
         static_cast<qint64>(handshake.runtimeApiVersion)},
        {QStringLiteral("legacy_runtime_capabilities"),
         handshake.legacyRuntimeCapabilities},
        {QStringLiteral("runtime_capabilities"),
         QJsonArray::fromStringList(
             handshake.runtimeCapabilities)},
        {QStringLiteral("device_capabilities_status"),
         deviceStatus},
        {QStringLiteral("device_capabilities"),
         QJsonArray::fromStringList(deviceCapabilities)},
    };
}

QByteArray humanCapabilities(const QJsonObject &data) {
    QString output = QStringLiteral(
        "Runtime API: %1\n"
        "Legacy runtime capabilities: %2\n"
        "Runtime capabilities:\n");
    output = output.arg(
        data.value(QStringLiteral("runtime_api_version"))
            .toInteger());
    output = output.arg(
        data.value(
                QStringLiteral("legacy_runtime_capabilities"))
                .toBool()
            ? QStringLiteral("yes")
            : QStringLiteral("no"));
    for (const QJsonValue &value :
         data.value(QStringLiteral("runtime_capabilities"))
             .toArray()) {
        output.append(QStringLiteral("  %1\n").arg(value.toString()));
    }
    output.append(QStringLiteral("Device capabilities: %1\n").arg(
        data.value(
                QStringLiteral("device_capabilities_status"))
            .toString()));
    for (const QJsonValue &value :
         data.value(QStringLiteral("device_capabilities"))
             .toArray()) {
        output.append(QStringLiteral("  %1\n").arg(value.toString()));
    }
    return output.toUtf8();
}

bool pathArgumentIsSafe(const QString &path) {
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) {
        return false;
    }
    for (const QChar character : path) {
        const uint codePoint = character.unicode();
        if (codePoint <= 0x1f || codePoint == 0x7f ||
            (codePoint >= 0x80 && codePoint <= 0x9f) ||
            codePoint == 0x061c ||
            codePoint == 0x200e || codePoint == 0x200f ||
            codePoint == 0x2028 || codePoint == 0x2029 ||
            (codePoint >= 0x202a && codePoint <= 0x202e) ||
            (codePoint >= 0x2066 && codePoint <= 0x2069)) {
            return false;
        }
    }
    return true;
}

RunResult writeReport(
    const Invocation &invocation,
    const QString &runtimeSnapshot,
    tryx::support_bundle::RuntimeSnapshotStatus runtimeStatus,
    const tryx::support_bundle::PublicationGuard &publicationGuard = {},
    Failure *publishFailure = nullptr) {
    if (!pathArgumentIsSafe(invocation.outputDirectory)) {
        return failureResult(
            invocation, Failure::UnsafeDestination);
    }

    const qint64 generatedAtUtcMs =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QString buildError;
    const QByteArray report =
        tryx::support_bundle::buildReportV1(
            runtimeSnapshot, runtimeStatus,
            generatedAtUtcMs, &buildError);
    if (report.isEmpty()) {
        return failureResult(invocation, Failure::Internal);
    }

    const auto writeResult =
        tryx::support_bundle::writeNewReport(
            QUrl::fromLocalFile(invocation.outputDirectory),
            tryx::support_bundle::generatedFileName(
                generatedAtUtcMs),
            report,
            publicationGuard);
    if (!writeResult.ok()) {
        if (writeResult.status ==
                tryx::support_bundle::WriteStatus::PublishRejected &&
            publishFailure &&
            *publishFailure != Failure::None) {
            return failureResult(invocation, *publishFailure);
        }
        return failureResult(
            invocation, writeFailure(writeResult.status));
    }

    QString status;
    switch (runtimeStatus) {
    case tryx::support_bundle::RuntimeSnapshotStatus::Available:
        status = QStringLiteral("available");
        break;
    case tryx::support_bundle::RuntimeSnapshotStatus::Unsupported:
        status = QStringLiteral("unsupported");
        break;
    case tryx::support_bundle::RuntimeSnapshotStatus::Unavailable:
        status = QStringLiteral("unavailable");
        break;
    }
    const QJsonObject data{
        {QStringLiteral("path"), writeResult.path},
        {QStringLiteral("runtime_snapshot_status"), status},
    };
    return successResult(
        invocation, data,
        QStringLiteral(
            "Support report: %1\nRuntime snapshot: %2\n")
            .arg(writeResult.path, status)
            .toUtf8());
}

RunResult writeOwnerGuardedReport(
    const Invocation &invocation,
    const QString &runtimeSnapshot,
    tryx::support_bundle::RuntimeSnapshotStatus runtimeStatus,
    const QDBusConnection &bus,
    const Deadline &deadline,
    const QString &owner) {
    Failure publishFailure = Failure::Internal;
    const auto publishGuard = [&]() {
        publishFailure = fenceOwner(bus, deadline, owner);
        return publishFailure == Failure::None;
    };
    return writeReport(
        invocation, runtimeSnapshot, runtimeStatus,
        publishGuard, &publishFailure);
}

RunResult runStatusOrOperations(
    const Invocation &invocation,
    const QDBusConnection &bus,
    const Deadline &deadline,
    const HandshakeResult &handshake) {
    const SnapshotResult snapshot = collectSupportSnapshot(
        bus, deadline, handshake);
    if (!snapshot.ok()) {
        return failureResult(invocation, snapshot.failure);
    }
    const Failure finalFence = fenceOwner(
        bus, deadline, handshake.owner);
    if (finalFence != Failure::None) {
        return failureResult(invocation, finalFence);
    }

    if (invocation.command == Command::Status) {
        const QJsonObject data = statusData(snapshot.root);
        return successResult(
            invocation, data, humanStatus(data));
    }
    const QJsonObject data = operationsData(snapshot.root);
    return successResult(
        invocation, data, humanOperations(data));
}

RunResult runCapabilities(
    const Invocation &invocation,
    const QDBusConnection &bus,
    const Deadline &deadline,
    const HandshakeResult &handshake) {
    QString deviceStatus = QStringLiteral("unsupported");
    QStringList deviceCapabilities;

    if (!handshake.legacyRuntimeCapabilities &&
        handshake.runtimeCapabilities.contains(
            tryxRuntimeDeviceCapabilitiesV1Token())) {
        const RuntimeCallResult connectionCall = callRuntime(
            bus, deadline, handshake.owner,
            QStringLiteral("GetConnectionSnapshot"),
            QStringLiteral("(tbbbbssssass)"));
        if (!connectionCall.ok()) {
            return failureResult(
                invocation, connectionCall.failure);
        }
        const QDBusReply<TryxRuntimeSnapshot> connectionReply(
            connectionCall.reply);
        if (!connectionReply.isValid()) {
            return failureResult(
                invocation, Failure::InvalidReply);
        }
        const TryxRuntimeSnapshot connection =
            connectionReply.value();
        const QString expectedIdentity =
            connection.serial.trimmed();
        const bool connected = connection.connected &&
            connection.printerClassConnected &&
            connection.printerClassDevicePresent &&
            !expectedIdentity.isEmpty() &&
            connection.revision != 0;
        if (!connected) {
            deviceStatus = QStringLiteral("not_connected");
        } else {
            const RuntimeCallResult deviceCall = callRuntime(
                bus, deadline, handshake.owner,
                QStringLiteral("GetDeviceCapabilitiesV1"),
                QStringLiteral("(usttas)"));
            if (!deviceCall.ok()) {
                return failureResult(
                    invocation, deviceCall.failure);
            }
            const QDBusReply<TryxRuntimeDeviceCapabilitiesV1>
                deviceReply(deviceCall.reply);
            if (!deviceReply.isValid()) {
                return failureResult(
                    invocation, Failure::InvalidReply);
            }
            const TryxRuntimeDeviceCapabilitiesV1 device =
                deviceReply.value();
            if (device.schemaVersion != 1 ||
                device.deviceIdentity.isEmpty() ||
                device.deviceIdentity != expectedIdentity ||
                device.connectionRevision != connection.revision ||
                device.physicalGeneration == 0) {
                return failureResult(
                    invocation, Failure::InvalidReply);
            }
            deviceCapabilities = canonicalIntersection(
                deviceCapabilityAllowlist(),
                tryxFilterDeviceCapabilities(
                    device.capabilities));
            deviceStatus = QStringLiteral("available");
        }
    }

    const Failure finalFence = fenceOwner(
        bus, deadline, handshake.owner);
    if (finalFence != Failure::None) {
        return failureResult(invocation, finalFence);
    }
    const QJsonObject data = capabilitiesData(
        handshake, deviceStatus, deviceCapabilities);
    return successResult(
        invocation, data, humanCapabilities(data));
}

RunResult runPrepareDowngradeV10(
    const Invocation &invocation,
    const QDBusConnection &bus,
    const Deadline &deadline,
    const HandshakeResult &handshake) {
    if (handshake.legacyRuntimeCapabilities ||
        !handshake.runtimeCapabilities.contains(
            tryxRuntimeDowngradeV10PreparationV1Token())) {
        return failureResult(invocation, Failure::Unsupported);
    }

    QDBusMessage request = makeRuntimeMethodCall(
        handshake.owner,
        QString::fromLatin1(kDowngradeV10PreparationMethod));
    Failure callFailure = Failure::Internal;
    const QDBusMessage reply = boundedCall(
        bus, request, deadline, &callFailure);
    if (callFailure != Failure::None) {
        return failureResult(invocation, callFailure);
    }

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (reply.errorName() ==
            QString::fromLatin1(kUnknownMethodError)) {
            const Failure fence = fenceOwner(
                bus, deadline, handshake.owner);
            return failureResult(
                invocation,
                fence == Failure::None
                    ? Failure::Unsupported
                    : fence);
        }
        if (reply.errorName() ==
                QString::fromLatin1(kServiceUnknownError) ||
            reply.errorName() ==
                QString::fromLatin1(kNameHasNoOwnerError)) {
            return failureResult(
                invocation, Failure::OwnerChanged);
        }
        const Failure transportFailure = callError(bus, reply);
        if (transportFailure == Failure::Timeout ||
            transportFailure == Failure::SessionBusUnavailable) {
            return failureResult(invocation, transportFailure);
        }
        const Failure fence = fenceOwner(
            bus, deadline, handshake.owner);
        return failureResult(
            invocation,
            fence == Failure::None
                ? Failure::DowngradeBlocked
                : fence);
    }

    if (reply.type() != QDBusMessage::ReplyMessage ||
        reply.signature() != QStringLiteral("s") ||
        reply.arguments().size() != 1) {
        const Failure fence = fenceOwner(
            bus, deadline, handshake.owner);
        return failureResult(
            invocation,
            fence == Failure::None
                ? Failure::InvalidReply
                : fence);
    }
    const QDBusReply<QString> decoded(reply);
    const QString compatibilityState = decoded.value();
    if (!decoded.isValid() ||
        (compatibilityState !=
             QString::fromLatin1(kDowngradeStateEmpty) &&
         compatibilityState !=
             QString::fromLatin1(kDowngradeStateFullFrame))) {
        const Failure fence = fenceOwner(
            bus, deadline, handshake.owner);
        return failureResult(
            invocation,
            fence == Failure::None
                ? Failure::InvalidReply
                : fence);
    }

    const Failure ownerExit = waitForOwnerExit(
        bus, deadline, handshake.owner);
    if (ownerExit != Failure::None) {
        return failureResult(invocation, ownerExit);
    }
    const QJsonObject data{
        {QStringLiteral("compatibility_state"),
         compatibilityState},
    };
    return successResult(
        invocation,
        data,
        QStringLiteral(
            "Runtime downgrade v10 prepared: %1\n")
            .arg(compatibilityState)
            .toUtf8());
}

RunResult runAbortDowngradeV10(
    const Invocation &invocation,
    const QDBusConnection &bus,
    const Deadline &deadline) {
    const OwnerResult owner = resolveOwner(bus, deadline);
    if (owner.ok()) {
        return failureResult(
            invocation, Failure::DowngradeBlocked);
    }
    if (owner.failure != Failure::RuntimeUnavailable) {
        return failureResult(invocation, owner.failure);
    }

    const auto aborted = tryx::RuntimeDowngradeStore()
        .abortForInstalledExecutable();
    if (aborted.status !=
        tryx::RuntimeDowngradeStore::AbortStatus::Aborted) {
        return failureResult(
            invocation, Failure::DowngradeBlocked);
    }
    return successResult(
        invocation,
        QJsonObject{{QStringLiteral("aborted"), true}},
        QByteArrayLiteral(
            "Runtime downgrade v10 preparation aborted\n"));
}

RunResult runSupportReport(
    const Invocation &invocation,
    const QDBusConnection &bus,
    const Deadline &deadline) {
    if (!bus.isConnected()) {
        return writeReport(
            invocation, {},
            tryx::support_bundle::
                RuntimeSnapshotStatus::Unavailable);
    }

    const OwnerResult owner = resolveOwner(bus, deadline);
    if (owner.failure == Failure::RuntimeUnavailable ||
        owner.failure == Failure::SessionBusUnavailable) {
        return writeReport(
            invocation, {},
            tryx::support_bundle::
                RuntimeSnapshotStatus::Unavailable);
    }
    if (!owner.ok()) {
        return failureResult(invocation, owner.failure);
    }

    const HandshakeResult handshake = performHandshake(
        bus, deadline, owner.owner);
    if (handshake.failure == Failure::IncompatibleRuntime) {
        return writeOwnerGuardedReport(
            invocation, {},
            tryx::support_bundle::
                RuntimeSnapshotStatus::Unsupported,
            bus, deadline, owner.owner);
    }
    if (!handshake.ok()) {
        return failureResult(invocation, handshake.failure);
    }
    if (handshake.legacyRuntimeCapabilities ||
        !handshake.runtimeCapabilities.contains(
            tryxRuntimeSupportSnapshotV1Token())) {
        return writeOwnerGuardedReport(
            invocation, {},
            tryx::support_bundle::
                RuntimeSnapshotStatus::Unsupported,
            bus, deadline, handshake.owner);
    }

    const RuntimeCallResult snapshotCall = callRuntime(
        bus, deadline, handshake.owner,
        QStringLiteral("GetSupportSnapshotV1"),
        QStringLiteral("s"));
    if (!snapshotCall.ok()) {
        return failureResult(invocation, snapshotCall.failure);
    }
    const QDBusReply<QString> snapshotReply(snapshotCall.reply);
    if (!snapshotReply.isValid() ||
        !tryx::supportSnapshotV1IsValid(snapshotReply.value())) {
        return failureResult(invocation, Failure::InvalidReply);
    }
    const Failure finalFence = fenceOwner(
        bus, deadline, handshake.owner);
    if (finalFence != Failure::None) {
        return failureResult(invocation, finalFence);
    }
    return writeOwnerGuardedReport(
        invocation, snapshotReply.value(),
        tryx::support_bundle::RuntimeSnapshotStatus::Available,
        bus, deadline, handshake.owner);
}

}  // namespace

QByteArray helpText() {
    return QByteArrayLiteral(
        "Usage: tryx [--json] <command>\n"
        "\n"
        "Commands:\n"
        "  status\n"
        "  capabilities\n"
        "  operations\n"
        "  prepare-downgrade-v10\n"
        "  abort-downgrade-v10\n"
        "  support-report --output-dir ABSOLUTE_DIR\n");
}

QByteArray versionText() {
    return QByteArrayLiteral("tryx " TRYX_APP_VERSION "\n");
}

RunResult runWithoutSessionBus(const QStringList &arguments) {
    Invocation invocation;
    if (!parseInvocation(arguments, &invocation)) {
        RunResult result;
        result.exitCode = 2;
        result.standardError = helpText();
        return result;
    }
    if (invocation.command == Command::SupportReport) {
        return writeReport(
            invocation, {},
            tryx::support_bundle::
                RuntimeSnapshotStatus::Unavailable);
    }
    return failureResult(
        invocation, Failure::SessionBusUnavailable);
}

RunResult run(
    const QStringList &arguments,
    const QDBusConnection &bus,
    int dbusDeadlineMs) {
    Invocation invocation;
    if (!parseInvocation(arguments, &invocation)) {
        RunResult result;
        result.exitCode = 2;
        result.standardError = helpText();
        return result;
    }

    if (invocation.command == Command::SupportReport &&
        !pathArgumentIsSafe(invocation.outputDirectory)) {
        return failureResult(
            invocation, Failure::UnsafeDestination);
    }

    if (!bus.isConnected()) {
        if (invocation.command == Command::SupportReport) {
            return writeReport(
                invocation, {},
                tryx::support_bundle::
                    RuntimeSnapshotStatus::Unavailable);
        }
        return failureResult(
            invocation, Failure::SessionBusUnavailable);
    }
    if (invocation.command == Command::AbortDowngradeV10) {
        const Deadline deadline(dbusDeadlineMs);
        return runAbortDowngradeV10(
            invocation, bus, deadline);
    }
    if (invocation.command == Command::SupportReport) {
        const Deadline deadline(dbusDeadlineMs);
        return runSupportReport(
            invocation, bus, deadline);
    }

    registerTryxRuntimeMetaTypes();
    const Deadline deadline(dbusDeadlineMs);
    const HandshakeResult handshake = performHandshake(
        bus, deadline);
    if (!handshake.ok()) {
        return failureResult(invocation, handshake.failure);
    }
    if (invocation.command == Command::Capabilities) {
        return runCapabilities(
            invocation, bus, deadline, handshake);
    }
    if (invocation.command == Command::PrepareDowngradeV10) {
        return runPrepareDowngradeV10(
            invocation, bus, deadline, handshake);
    }
    return runStatusOrOperations(
        invocation, bus, deadline, handshake);
}

#ifdef TRYX_CLI_TESTING
QDBusMessage runtimeMethodCallForTesting(
    const QString &owner,
    const QString &method) {
    return makeRuntimeMethodCall(owner, method);
}
#endif

}  // namespace tryx::cli
