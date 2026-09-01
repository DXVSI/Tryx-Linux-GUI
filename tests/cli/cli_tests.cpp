#include <QDir>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>

#include "runtimecontract.h"
#include "runtimedowngradestore.h"
#include "sessionbusguard.h"
#include "supportbundle.h"
#include "supportsnapshot.h"
#include "tryxclirunner.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr auto kSerialCanary =
    "SERIAL-SECRET-CANARY";
constexpr auto kMediaCanary =
    "MEDIA-PATH-SECRET-CANARY";
constexpr auto kDiagnosticCanary =
    "DIAGNOSTIC-SECRET-CANARY";
constexpr auto kDowngradeMarkerCanary =
    "DOWNGRADE-MARKER-SECRET-CANARY";
struct ProcessResult {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray standardOutput;
    QByteArray standardError;
    QString processError;
};

class ScopedSocketDescriptor final {
public:
    explicit ScopedSocketDescriptor(int descriptor)
        : descriptor_(descriptor) {}

    ~ScopedSocketDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedSocketDescriptor(const ScopedSocketDescriptor &) = delete;
    ScopedSocketDescriptor &operator=(
        const ScopedSocketDescriptor &) = delete;

    int get() const {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

QString unavailableBusAddress() {
    return QStringLiteral("unix:path=%1")
        .arg(QDir::temp().filePath(
            QStringLiteral("tryx-cli-no-bus-%1")
                .arg(QCoreApplication::applicationPid())));
}

QProcessEnvironment headlessEnvironment(bool unavailableBus) {
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    environment.remove(QStringLiteral("DBUS_STARTER_ADDRESS"));
    environment.remove(QStringLiteral("DBUS_STARTER_BUS_TYPE"));
    environment.insert(
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("tryx-cli-must-not-load-qpa"));
    if (unavailableBus) {
        environment.insert(
            QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
            unavailableBusAddress());
    }
    return environment;
}

void configureCliProcess(
    QProcess *process,
    const QStringList &arguments,
    const QProcessEnvironment &environment =
        QProcessEnvironment::systemEnvironment()) {
    process->setProcessEnvironment(environment);
    process->setProgram(QStringLiteral(TRYX_CLI_BINARY));
    process->setArguments(arguments);
}

ProcessResult collectFinishedCli(QProcess *process) {
    ProcessResult result;
    result.started = true;
    result.finished = process->state() == QProcess::NotRunning;
    result.exitCode = process->exitCode();
    result.exitStatus = process->exitStatus();
    result.standardOutput = process->readAllStandardOutput();
    result.standardError = process->readAllStandardError();
    result.processError = process->errorString();
    return result;
}

ProcessResult runCli(
    const QStringList &arguments,
    const QProcessEnvironment &environment =
        QProcessEnvironment::systemEnvironment()) {
    QProcess process;
    configureCliProcess(&process, arguments, environment);
    process.start();

    ProcessResult result;
    result.started = process.waitForStarted(3000);
    if (!result.started) {
        result.processError = process.errorString();
        return result;
    }
    result.finished = process.waitForFinished(10000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(1000);
    }
    ProcessResult finished = collectFinishedCli(&process);
    finished.started = result.started;
    finished.finished = result.finished;
    return finished;
}

QJsonObject parseSingleJsonObject(const QByteArray &bytes) {
    if (!bytes.endsWith('\n') ||
        bytes.left(bytes.size() - 1).contains('\n')) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return {};
    }
    return document.object();
}

void verifyEnvelope(
    const QJsonObject &root,
    const QString &command,
    bool ok) {
    QCOMPARE(
        root.keys(),
        QStringList({
            QStringLiteral("command"),
            QStringLiteral("data"),
            QStringLiteral("error"),
            QStringLiteral("ok"),
            QStringLiteral("schema_version"),
        }));
    QCOMPARE(root.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("command")).toString(), command);
    QCOMPARE(root.value(QStringLiteral("ok")).toBool(), ok);
    if (ok) {
        QVERIFY(root.value(QStringLiteral("data")).isObject());
        QVERIFY(root.value(QStringLiteral("error")).isNull());
    } else {
        QVERIFY(root.value(QStringLiteral("data")).isNull());
        QVERIFY(root.value(QStringLiteral("error")).isObject());
    }
}

void verifySessionBusUnavailable(const ProcessResult &result) {
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 3);
    QVERIFY(result.standardError.isEmpty());

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    const QJsonObject error =
        root.value(QStringLiteral("error")).toObject();
    QCOMPARE(error.keys(), QStringList{QStringLiteral("code")});
    QCOMPARE(
        error.value(QStringLiteral("code")).toString(),
        QStringLiteral("SESSION_BUS_UNAVAILABLE"));
}

QStringList jsonStringList(const QJsonValue &value) {
    QStringList result;
    if (!value.isArray()) {
        return result;
    }
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isString()) {
            return {};
        }
        result.append(entry.toString());
    }
    return result;
}

TryxRuntimeSnapshot connectedSnapshot() {
    TryxRuntimeSnapshot snapshot;
    snapshot.revision = 41;
    snapshot.connected = true;
    snapshot.printerClassConnected = true;
    snapshot.printerClassDevicePresent = true;
    snapshot.displaySessionActive = true;
    snapshot.productId = QStringLiteral("391a:1021");
    snapshot.serial = QString::fromLatin1(kSerialCanary);
    snapshot.firmware = QStringLiteral("1.4.7");
    snapshot.appVersion = QStringLiteral("3.2.1");
    snapshot.mediaFiles = {QString::fromLatin1(kMediaCanary)};
    snapshot.diagnostic = QString::fromLatin1(kDiagnosticCanary);
    return snapshot;
}

QString supportSnapshotWithCanaries() {
    tryx::SupportSnapshotSourceV1 source;
    source.generatedAtUtcMs =
        QDateTime::fromString(
            QStringLiteral("2026-08-31T12:00:00.000Z"),
            Qt::ISODateWithMs)
            .toMSecsSinceEpoch();
    source.runtimeVersion = QStringLiteral("2.2.0");
    source.runtimeApiVersion = 8;
    source.connection = connectedSnapshot();
    source.physicalGeneration = 73;
    source.recoveryRequired = true;
    source.firmwareRecoveryInterlockActive = false;
    source.mediaCatalogEntryCount = 7;
    source.artifactCount = 3;
    source.operationCount = 47;
    source.retryCandidatePresent = true;
    source.retryDispatchPresent = false;
    source.retryCleanupPendingCount = 2;
    source.deleteRecoveryPresent = true;
    source.replaceRecoveryPresent = false;
    for (int index = 0; index < 47; ++index) {
        TryxRuntimeOperationInfo operation;
        operation.id =
            QStringLiteral("operation-%1-%2")
                .arg(index)
                .arg(QString::fromLatin1(kSerialCanary));
        operation.parentId = QString::fromLatin1(kDiagnosticCanary);
        operation.kind = index % 2 == 0
            ? QStringLiteral("Upload")
            : QStringLiteral("Apply");
        operation.state = index % 3 == 0
            ? QStringLiteral("Completed")
            : QStringLiteral("Running");
        operation.stage = index % 2 == 0
            ? QStringLiteral("Uploading")
            : QStringLiteral("Applying");
        operation.errorCategory = QStringLiteral("None");
        operation.terminalOutcome =
            index % 3 == 0
            ? QStringLiteral("Succeeded")
            : QStringLiteral("NotStarted");
        operation.primaryErrorCategory =
            QStringLiteral("None");
        operation.retryMode = QStringLiteral("None");
        operation.subject = QString::fromLatin1(kMediaCanary);
        operation.resultName = QString::fromLatin1(kMediaCanary);
        operation.message = QString::fromLatin1(kDiagnosticCanary);
        operation.attempt =
            static_cast<quint32>(index + 1);
        operation.applyAfterUpload = index % 2 == 0;
        source.operations.append(operation);
    }
    return tryx::buildSupportSnapshotV1(source);
}

class FakeCliRuntime final
    : public QObject,
      protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "org.tryx.Panorama.Manager2")

public:
    enum class ApiReplyMode {
        Normal,
        Delayed,
    };

    enum class RuntimeCapabilityReplyMode {
        Normal,
        AccessDenied,
    };

    enum class SupportReplyMode {
        Normal,
        Delayed,
    };

    enum class DowngradeReplyMode {
        Normal,
        Refused,
        UnknownMethod,
        InvalidSignature,
    };

    quint32 apiVersion = tryxRuntimeApiVersion();
    QStringList runtimeCapabilities = {
        tryxRuntimeSupportSnapshotV1Token(),
        QStringLiteral("runtime.unknown.v1"),
        tryxRuntimeDeviceCapabilitiesV1Token(),
        tryxRuntimeSupportSnapshotV1Token(),
    };
    TryxRuntimeSnapshot connection = connectedSnapshot();
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilities = {
        1,
        QString::fromLatin1(kSerialCanary),
        41,
        73,
        {
            tryxDeviceMediaCatalogV1Token(),
            QStringLiteral("device.unknown.v1"),
            tryxDeviceMediaUploadV1Token(),
            tryxDeviceMediaCatalogV1Token(),
        },
    };
    QString supportSnapshot = supportSnapshotWithCanaries();
    ApiReplyMode apiReplyMode = ApiReplyMode::Normal;
    RuntimeCapabilityReplyMode runtimeCapabilityReplyMode =
        RuntimeCapabilityReplyMode::Normal;
    SupportReplyMode supportReplyMode = SupportReplyMode::Normal;
    DowngradeReplyMode downgradeReplyMode =
        DowngradeReplyMode::Normal;
    QString downgradeCompatibilityState = QStringLiteral("Empty");
    bool releaseServiceNameAfterDowngradeReply = false;
    std::atomic_int apiCalls{0};
    std::atomic_int runtimeCapabilityCalls{0};
    std::atomic_int connectionSnapshotCalls{0};
    std::atomic_int deviceCapabilityCalls{0};
    std::atomic_int supportSnapshotCalls{0};
    std::atomic_int downgradePreparationCalls{0};
    QDBusMessage delayedApiMessage;
    QDBusMessage delayedSupportMessage;

    bool sendDelayedSupportReply(
        const QDBusConnection &connection,
        const QString &snapshot) {
        if (delayedSupportMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedSupportMessage.createReply(
                QVariantList{QVariant::fromValue(snapshot)}));
        delayedSupportMessage = {};
        return sent;
    }

public slots:
    quint32 GetRuntimeApiVersion() {
        apiCalls.fetch_add(1, std::memory_order_release);
        if (apiReplyMode == ApiReplyMode::Delayed) {
            setDelayedReply(true);
            delayedApiMessage = message();
            return 0;
        }
        return apiVersion;
    }

    QStringList GetRuntimeCapabilities() {
        runtimeCapabilityCalls.fetch_add(
            1, std::memory_order_release);
        if (runtimeCapabilityReplyMode ==
            RuntimeCapabilityReplyMode::AccessDenied) {
            sendErrorReply(
                QDBusError::AccessDenied,
                QString::fromLatin1(kDiagnosticCanary));
            return {};
        }
        return runtimeCapabilities;
    }

    TryxRuntimeSnapshot GetConnectionSnapshot() {
        connectionSnapshotCalls.fetch_add(
            1, std::memory_order_release);
        return connection;
    }

    TryxRuntimeDeviceCapabilitiesV1
    GetDeviceCapabilitiesV1() {
        deviceCapabilityCalls.fetch_add(
            1, std::memory_order_release);
        return deviceCapabilities;
    }

    QString GetSupportSnapshotV1() {
        supportSnapshotCalls.fetch_add(
            1, std::memory_order_release);
        if (supportReplyMode == SupportReplyMode::Delayed) {
            setDelayedReply(true);
            delayedSupportMessage = message();
            return {};
        }
        return supportSnapshot;
    }

    QString PrepareRuntimeDowngradeV10() {
        downgradePreparationCalls.fetch_add(
            1, std::memory_order_release);
        switch (downgradeReplyMode) {
        case DowngradeReplyMode::Normal:
            if (releaseServiceNameAfterDowngradeReply) {
                QDBusContext::connection().unregisterService(
                    tryxRuntimeServiceName());
            }
            return downgradeCompatibilityState;
        case DowngradeReplyMode::Refused:
            sendErrorReply(
                QDBusError::AccessDenied,
                QString::fromLatin1(kDiagnosticCanary));
            return {};
        case DowngradeReplyMode::UnknownMethod:
            sendErrorReply(
                QDBusError::UnknownMethod,
                QString::fromLatin1(kDiagnosticCanary));
            return {};
        case DowngradeReplyMode::InvalidSignature:
            setDelayedReply(true);
            QDBusContext::connection().send(
                message().createReply(
                    QVariantList{QVariant::fromValue(true)}));
            return {};
        }
        return {};
    }
};

class LegacyCliRuntime final
    : public QObject,
      protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO(
        "D-Bus Interface",
        "org.tryx.Panorama.Manager2")

public:
    std::atomic_int apiCalls{0};
    std::atomic_int runtimeCapabilityCalls{0};

public slots:
    quint32 GetRuntimeApiVersion() {
        apiCalls.fetch_add(1, std::memory_order_release);
        return tryxRuntimeApiVersion();
    }

    QStringList GetRuntimeCapabilities() {
        runtimeCapabilityCalls.fetch_add(
            1, std::memory_order_release);
        sendErrorReply(
            QDBusError::UnknownMethod,
            QString::fromLatin1(kDiagnosticCanary));
        return {};
    }
};

class ScopedRuntimeService final {
public:
    explicit ScopedRuntimeService(QObject *object)
        : object_(object),
          connectionName_(
              QStringLiteral("tryx-cli-test-%1")
                  .arg(QUuid::createUuid().toString(
                      QUuid::WithoutBraces))),
          connection_(QDBusConnection::connectToBus(
              QDBusConnection::SessionBus,
              connectionName_)) {
        worker_.start();
        object_->moveToThread(&worker_);
    }

    ~ScopedRuntimeService() {
        stop();
        QThread *applicationThread =
            QCoreApplication::instance()->thread();
        QMetaObject::invokeMethod(
            object_,
            [object = object_, applicationThread]() {
                object->moveToThread(applicationThread);
            },
            Qt::BlockingQueuedConnection);
        worker_.quit();
        worker_.wait();
        QDBusConnection::disconnectFromBus(connectionName_);
    }

    bool start() {
        if (!connection_.isConnected()) {
            return false;
        }
        objectRegistered_ = connection_.registerObject(
            tryxRuntimeObjectPath(), object_,
            QDBusConnection::ExportAllSlots);
        if (!objectRegistered_) {
            return false;
        }
        serviceRegistered_ = connection_.registerService(
            tryxRuntimeServiceName());
        return serviceRegistered_;
    }

    bool releaseServiceName() {
        if (!serviceRegistered_) {
            return true;
        }
        const bool released = connection_.unregisterService(
            tryxRuntimeServiceName());
        if (released) {
            serviceRegistered_ = false;
        }
        return released;
    }

    bool disconnectOwner() {
        stop();
        QDBusConnection::disconnectFromBus(connectionName_);
        connection_ = QDBusConnection(QString());
        return !connection_.isConnected();
    }

    void stop() {
        releaseServiceName();
        if (objectRegistered_) {
            connection_.unregisterObject(
                tryxRuntimeObjectPath());
            objectRegistered_ = false;
        }
    }

    const QDBusConnection &connection() const {
        return connection_;
    }

    template <typename Function>
    bool invoke(Function &&function) {
        return QMetaObject::invokeMethod(
            object_, std::forward<Function>(function),
            Qt::BlockingQueuedConnection);
    }

private:
    QObject *object_ = nullptr;
    QString connectionName_;
    QDBusConnection connection_;
    QThread worker_;
    bool objectRegistered_ = false;
    bool serviceRegistered_ = false;
};

void verifyNoSensitiveCanaries(const ProcessResult &result) {
    for (const QByteArray &canary :
         {
             QByteArray(kSerialCanary),
             QByteArray(kMediaCanary),
             QByteArray(kDiagnosticCanary),
         }) {
        QVERIFY2(
            !result.standardOutput.contains(canary),
            canary.constData());
        QVERIFY2(
            !result.standardError.contains(canary),
            canary.constData());
    }
}

ProcessResult processResultFromDirectRun(
    const tryx::cli::RunResult &direct) {
    ProcessResult result;
    result.started = true;
    result.finished = true;
    result.exitCode = direct.exitCode;
    result.exitStatus = QProcess::NormalExit;
    result.standardOutput = direct.standardOutput;
    result.standardError = direct.standardError;
    return result;
}

QStringList directoryEntries(const QString &path) {
    return QDir(path).entryList(
        QDir::AllEntries | QDir::Hidden |
            QDir::NoDotAndDotDot);
}

}  // namespace

class TryxCliTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void versionAndHelpNeedNoBusOrDisplay();
    void downgradePreparationFormatsHumanAndJson_data();
    void downgradePreparationFormatsHumanAndJson();
    void noBusDowngradePreparationReturnsStableError();
    void downgradePreparationRequiresCapability();
    void downgradePreparationUnknownMethodIsUnsupported();
    void downgradePreparationRefusalUsesFixedError_data();
    void downgradePreparationRefusalUsesFixedError();
    void downgradePreparationRejectsInvalidReply_data();
    void downgradePreparationRejectsInvalidReply();
    void downgradePreparationRejectsInvalidSignature();
    void downgradePreparationRequiresOwnerExit();
    void downgradePreparationRejectsReplacementOwner();
    void noBusAbortDowngradeReturnsStableError();
    void abortDowngradeRejectsActiveOwnerWithoutTouchingMarker();
    void abortDowngradeMissingMarkerIsBlocked();
    void abortDowngradeRemovesExactOfflineMarker_data();
    void abortDowngradeRemovesExactOfflineMarker();
    void invalidArgumentsUseFixedUsage();
    void noBusStatusReturnsStableJsonError();
    void localSessionBusGuardAcceptsCurrentIsolatedBus();
    void unsafeSessionBusAddressesFailClosed_data();
    void unsafeSessionBusAddressesFailClosed();
    void unsafeFilesystemSessionBusFailsClosed();
    void unsetSessionBusDoesNotAutolaunch();
    void unsafeOutputDirectoryControlsFailClosed_data();
    void unsafeOutputDirectoryControlsFailClosed();
    void noOwnerStatusReturnsStableJsonError();
    void hostOnlyReportWorksWithoutBus();
    void hostOnlyReportWorksForUnsupportedRuntime();
    void statusUsesValidatedSupportSnapshot();
    void operationsUseBoundedSupportSnapshotTail();
    void capabilitiesUseCanonicalAllowlistsAndContext();
    void runtimeRequestTargetsExactOwnerWithoutAutostart();
    void legacyUnknownMethodCapabilitiesSucceed();
    void apiMismatchReturnsStableError();
    void malformedSupportSnapshotFailsWithoutLeak();
    void runtimeCapabilityErrorFailsWithoutLeak();
    void invalidDeviceContextFailsClosed_data();
    void invalidDeviceContextFailsClosed();
    void advertisedSupportReportIncludesValidatedRuntimeSnapshot();
    void invalidAdvertisedSupportReportCreatesNoFile();
    void delayedSnapshotReplyAfterOwnerLossFailsClosed();
    void shortInternalDeadlineReturnsTimeout();
    void ownerLossBeforeSupportReportPublishCreatesNoFile();
};

void TryxCliTests::initTestCase() {
    registerTryxRuntimeMetaTypes();
    QVERIFY2(
        QDBusConnection::sessionBus().isConnected(),
        "CLI tests require a D-Bus session");
}

void TryxCliTests::versionAndHelpNeedNoBusOrDisplay() {
    const QProcessEnvironment environment =
        headlessEnvironment(true);

    const ProcessResult version =
        runCli({QStringLiteral("--version")}, environment);
    QVERIFY2(version.started, qPrintable(version.processError));
    QVERIFY2(version.finished, qPrintable(version.processError));
    QCOMPARE(version.exitStatus, QProcess::NormalExit);
    QCOMPARE(version.exitCode, 0);
    QCOMPARE(
        version.standardOutput,
        QByteArrayLiteral("tryx " TRYX_EXPECTED_VERSION "\n"));
    QVERIFY(version.standardError.isEmpty());

    const ProcessResult help =
        runCli({QStringLiteral("--help")}, environment);
    QVERIFY2(help.started, qPrintable(help.processError));
    QVERIFY2(help.finished, qPrintable(help.processError));
    QCOMPARE(help.exitStatus, QProcess::NormalExit);
    QCOMPARE(help.exitCode, 0);
    QVERIFY(help.standardOutput.startsWith(
        QByteArrayLiteral("Usage: tryx")));
    QVERIFY(help.standardOutput.contains(
        QByteArrayLiteral("support-report")));
    QVERIFY(help.standardOutput.contains(
        QByteArrayLiteral("prepare-downgrade-v10")));
    QVERIFY(help.standardOutput.contains(
        QByteArrayLiteral("abort-downgrade-v10")));
    QVERIFY(help.standardError.isEmpty());
}

void TryxCliTests::downgradePreparationFormatsHumanAndJson_data() {
    QTest::addColumn<bool>("json");
    QTest::addColumn<QString>("compatibilityState");

    QTest::newRow("human-empty")
        << false << QStringLiteral("Empty");
    QTest::newRow("json-full-frame")
        << true << QStringLiteral("FullFrame");
}

void TryxCliTests::downgradePreparationFormatsHumanAndJson() {
    QFETCH(bool, json);
    QFETCH(QString, compatibilityState);

    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.downgradeCompatibilityState = compatibilityState;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QStringList arguments;
    if (json) {
        arguments.append(QStringLiteral("--json"));
    }
    arguments.append(QStringLiteral("prepare-downgrade-v10"));
    QProcess process;
    configureCliProcess(
        &process, arguments, headlessEnvironment(false));
    process.start();
    QVERIFY2(process.waitForStarted(3000),
             qPrintable(process.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.downgradePreparationCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(service.disconnectOwner());
    QTRY_VERIFY_WITH_TIMEOUT(
        process.state() == QProcess::NotRunning, 3000);

    const ProcessResult result = collectFinishedCli(&process);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    verifyNoSensitiveCanaries(result);
    if (!json) {
        QCOMPARE(result.standardOutput,
                 QStringLiteral(
                     "Runtime downgrade v10 prepared: %1\n")
                     .arg(compatibilityState)
                     .toUtf8());
        QVERIFY(result.standardError.isEmpty());
    } else {
        QVERIFY(result.standardError.isEmpty());
        const QJsonObject root =
            parseSingleJsonObject(result.standardOutput);
        QVERIFY(!root.isEmpty());
        verifyEnvelope(
            root,
            QStringLiteral("prepare-downgrade-v10"),
            true);
        const QJsonObject data =
            root.value(QStringLiteral("data")).toObject();
        QCOMPARE(
            data.keys(),
            QStringList{QStringLiteral("compatibility_state")});
        QCOMPARE(
            data.value(QStringLiteral("compatibility_state"))
                .toString(),
            compatibilityState);
    }
    QCOMPARE(runtime.apiCalls.load(), 1);
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 1);
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::
noBusDowngradePreparationReturnsStableError() {
    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(true));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 3);
    QVERIFY(result.standardError.isEmpty());

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("SESSION_BUS_UNAVAILABLE"));
}

void TryxCliTests::downgradePreparationRequiresCapability() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 6);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("UNSUPPORTED"));
    QCOMPARE(runtime.downgradePreparationCalls.load(), 0);
}

void TryxCliTests::
downgradePreparationUnknownMethodIsUnsupported() {
    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.downgradeReplyMode =
        FakeCliRuntime::DowngradeReplyMode::UnknownMethod;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 6);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("UNSUPPORTED"));
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::
downgradePreparationRefusalUsesFixedError_data() {
    QTest::addColumn<bool>("json");

    QTest::newRow("human") << false;
    QTest::newRow("json") << true;
}

void TryxCliTests::downgradePreparationRefusalUsesFixedError() {
    QFETCH(bool, json);

    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.downgradeReplyMode =
        FakeCliRuntime::DowngradeReplyMode::Refused;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QStringList arguments;
    if (json) {
        arguments.append(QStringLiteral("--json"));
    }
    arguments.append(QStringLiteral("prepare-downgrade-v10"));
    const ProcessResult result = runCli(
        arguments, headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 14);
    verifyNoSensitiveCanaries(result);
    if (!json) {
        QVERIFY(result.standardOutput.isEmpty());
        QCOMPARE(
            result.standardError,
            QByteArrayLiteral(
                "The TRYX runtime cannot prepare this downgrade safely\n"));
    } else {
        QVERIFY(result.standardError.isEmpty());
        const QJsonObject root =
            parseSingleJsonObject(result.standardOutput);
        QVERIFY(!root.isEmpty());
        verifyEnvelope(
            root,
            QStringLiteral("prepare-downgrade-v10"),
            false);
        QCOMPARE(
            root.value(QStringLiteral("error"))
                .toObject()
                .value(QStringLiteral("code"))
                .toString(),
            QStringLiteral("DOWNGRADE_BLOCKED"));
    }
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::
downgradePreparationRejectsInvalidReply_data() {
    QTest::addColumn<QString>("reply");

    QTest::newRow("empty") << QString();
    QTest::newRow("split-frame") << QStringLiteral("SplitFrame");
    QTest::newRow("diagnostic-canary")
        << QString::fromLatin1(kDiagnosticCanary);
}

void TryxCliTests::downgradePreparationRejectsInvalidReply() {
    QFETCH(QString, reply);

    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.downgradeCompatibilityState = reply;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 10);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INVALID_REPLY"));
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::
downgradePreparationRejectsInvalidSignature() {
    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.downgradeReplyMode =
        FakeCliRuntime::DowngradeReplyMode::InvalidSignature;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 10);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INVALID_REPLY"));
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::downgradePreparationRequiresOwnerExit() {
    FakeCliRuntime runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    runtime.releaseServiceNameAfterDowngradeReply = true;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const tryx::cli::RunResult direct = tryx::cli::run(
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        QDBusConnection::sessionBus(), 300);
    const ProcessResult result = processResultFromDirectRun(direct);
    QCOMPARE(result.exitCode, 7);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("TIMEOUT"));
    QCOMPARE(runtime.downgradePreparationCalls.load(), 1);
}

void TryxCliTests::
downgradePreparationRejectsReplacementOwner() {
    FakeCliRuntime originalRuntime;
    originalRuntime.runtimeCapabilities.append(
        tryxRuntimeDowngradeV10PreparationV1Token());
    ScopedRuntimeService originalService(&originalRuntime);
    QVERIFY(originalService.start());

    QProcess process;
    configureCliProcess(
        &process,
        {
            QStringLiteral("--json"),
            QStringLiteral("prepare-downgrade-v10"),
        },
        headlessEnvironment(false));
    process.start();
    QVERIFY2(process.waitForStarted(3000),
             qPrintable(process.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(
        originalRuntime.downgradePreparationCalls.load(
            std::memory_order_acquire),
        1, 3000);

    QVERIFY(originalService.releaseServiceName());
    QTest::qWait(25);
    FakeCliRuntime replacementRuntime;
    ScopedRuntimeService replacementService(&replacementRuntime);
    QVERIFY(replacementService.start());
    QTRY_VERIFY_WITH_TIMEOUT(
        process.state() == QProcess::NotRunning, 3000);

    const ProcessResult result = collectFinishedCli(&process);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 8);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("prepare-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("OWNER_CHANGED"));
}

void TryxCliTests::noBusAbortDowngradeReturnsStableError() {
    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("abort-downgrade-v10"),
        },
        headlessEnvironment(true));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 3);
    QVERIFY(result.standardError.isEmpty());

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("abort-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("SESSION_BUS_UNAVAILABLE"));
}

void TryxCliTests::
abortDowngradeRejectsActiveOwnerWithoutTouchingMarker() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const QString storeDirectory = QDir(state.path()).filePath(
        QStringLiteral(
            "tryx-panorama-manager-downgrade-v10"));
    QVERIFY(QDir().mkpath(storeDirectory));
    QVERIFY(QFile::setPermissions(
        storeDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QString markerPath = QDir(storeDirectory).filePath(
        QStringLiteral("runtime-downgrade-v10.json"));
    QFile marker(markerPath);
    QVERIFY(marker.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    const QByteArray markerBytes =
        QByteArray(kDowngradeMarkerCanary);
    QCOMPARE(marker.write(markerBytes), markerBytes.size());
    marker.close();
    QVERIFY(QFile::setPermissions(
        markerPath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner));

    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());
    QProcessEnvironment environment = headlessEnvironment(false);
    environment.insert(
        QStringLiteral("XDG_STATE_HOME"), state.path());
    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("abort-downgrade-v10"),
        },
        environment);
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 14);
    QVERIFY(result.standardError.isEmpty());
    QVERIFY(!result.standardOutput.contains(markerBytes));

    QFile preservedMarker(markerPath);
    QVERIFY(preservedMarker.open(QIODevice::ReadOnly));
    QCOMPARE(preservedMarker.readAll(), markerBytes);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("abort-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("DOWNGRADE_BLOCKED"));
    QCOMPARE(runtime.apiCalls.load(), 0);
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 0);
}

void TryxCliTests::abortDowngradeMissingMarkerIsBlocked() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    QProcessEnvironment environment = headlessEnvironment(false);
    environment.insert(
        QStringLiteral("XDG_STATE_HOME"), state.path());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("abort-downgrade-v10"),
        },
        environment);
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 14);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root,
        QStringLiteral("abort-downgrade-v10"),
        false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("DOWNGRADE_BLOCKED"));
}

void TryxCliTests::
abortDowngradeRemovesExactOfflineMarker_data() {
    QTest::addColumn<bool>("json");

    QTest::newRow("human") << false;
    QTest::newRow("json") << true;
}

void TryxCliTests::abortDowngradeRemovesExactOfflineMarker() {
    QFETCH(bool, json);

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const QString storeDirectory = QDir(state.path()).filePath(
        QStringLiteral(
            "tryx-panorama-manager-downgrade-v10"));
    tryx::RuntimeDowngradeStore store(storeDirectory);
    QString identityDetail;
    const auto identity =
        tryx::RuntimeDowngradeStore::currentExecutableIdentity(
            &identityDetail);
    QVERIFY2(identity.isValid(), qPrintable(identityDetail));
    const auto persisted = store.persist(
        QStringLiteral("Empty"), 1, identity);
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));
    QVERIFY(QFileInfo::exists(store.markerPath()));

    QProcessEnvironment environment = headlessEnvironment(false);
    environment.insert(
        QStringLiteral("XDG_STATE_HOME"), state.path());
    QStringList arguments;
    if (json) {
        arguments.append(QStringLiteral("--json"));
    }
    arguments.append(QStringLiteral("abort-downgrade-v10"));
    const ProcessResult result = runCli(arguments, environment);
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    QVERIFY(!QFileInfo::exists(store.markerPath()));

    if (!json) {
        QCOMPARE(
            result.standardOutput,
            QByteArrayLiteral(
                "Runtime downgrade v10 preparation aborted\n"));
    } else {
        const QJsonObject root =
            parseSingleJsonObject(result.standardOutput);
        QVERIFY(!root.isEmpty());
        verifyEnvelope(
            root,
            QStringLiteral("abort-downgrade-v10"),
            true);
        const QJsonObject data =
            root.value(QStringLiteral("data")).toObject();
        QCOMPARE(
            data.keys(),
            QStringList{QStringLiteral("aborted")});
        QVERIFY(data.value(QStringLiteral("aborted")).toBool());
    }
}

void TryxCliTests::invalidArgumentsUseFixedUsage() {
    const QByteArray canary =
        QByteArrayLiteral("ARGUMENT-SECRET-CANARY");
    const ProcessResult result = runCli(
        {QStringLiteral("--unknown=") +
             QString::fromLatin1(canary)},
        headlessEnvironment(true));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 2);
    QVERIFY(result.standardOutput.isEmpty());
    QVERIFY(result.standardError.startsWith(
        QByteArrayLiteral("Usage: tryx")));
    QVERIFY(!result.standardError.contains(canary));
}

void TryxCliTests::noBusStatusReturnsStableJsonError() {
    const QByteArray busAddress =
        unavailableBusAddress().toUtf8();
    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(true));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 3);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    const QJsonObject error =
        root.value(QStringLiteral("error")).toObject();
    QCOMPARE(
        error.keys(),
        QStringList{QStringLiteral("code")});
    QCOMPARE(
        error.value(QStringLiteral("code")).toString(),
        QStringLiteral("SESSION_BUS_UNAVAILABLE"));
    QVERIFY(!result.standardOutput.contains(busAddress));
    QVERIFY(!result.standardError.contains(busAddress));
}

void TryxCliTests::localSessionBusGuardAcceptsCurrentIsolatedBus() {
    const QByteArray address =
        qgetenv("DBUS_SESSION_BUS_ADDRESS");
    QVERIFY(!address.isEmpty());
    QVERIFY(tryx::cli::localSessionBusAddressIsSafe(address));
}

void TryxCliTests::unsafeSessionBusAddressesFailClosed_data() {
    QTest::addColumn<QByteArray>("address");

    QTest::newRow("autolaunch")
        << QByteArrayLiteral("autolaunch:");
    QTest::newRow("tcp")
        << QByteArrayLiteral("tcp:host=127.0.0.1,port=1");
    QTest::newRow("nonce-tcp")
        << QByteArrayLiteral(
               "nonce-tcp:host=127.0.0.1,port=1,"
               "noncefile=/tmp/tryx-cli-nonce");
    QTest::newRow("unix-abstract")
        << QByteArrayLiteral(
               "unix:abstract=tryx-cli-review");
    QTest::newRow("system-socket")
        << QByteArrayLiteral(
               "unix:path=/run/dbus/system_bus_socket");

    const QByteArray currentAddress =
        qgetenv("DBUS_SESSION_BUS_ADDRESS");
    QVERIFY(!currentAddress.isEmpty());
    QTest::newRow("fallback-list")
        << currentAddress + QByteArrayLiteral(";autolaunch:");
}

void TryxCliTests::unsafeSessionBusAddressesFailClosed() {
    QFETCH(QByteArray, address);

    QVERIFY(!tryx::cli::localSessionBusAddressIsSafe(address));

    QProcessEnvironment environment = headlessEnvironment(false);
    environment.insert(
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QString::fromLatin1(address));
    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        environment);

    verifySessionBusUnavailable(result);
    QVERIFY(!result.standardOutput.contains(address));
    QVERIFY(!result.standardError.contains(address));
}

void TryxCliTests::unsafeFilesystemSessionBusFailsClosed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QByteArray encodedDirectory =
        QFile::encodeName(directory.path());
    QCOMPARE(
        ::chmod(
            encodedDirectory.constData(),
            S_IRWXU | S_IRWXG | S_IRWXO),
        0);

    struct stat directoryStatus {};
    QCOMPARE(
        ::lstat(encodedDirectory.constData(), &directoryStatus),
        0);
    QCOMPARE(
        directoryStatus.st_mode & 07777,
        static_cast<mode_t>(0777));
    QVERIFY((directoryStatus.st_mode & S_ISVTX) == 0);
    QCOMPARE(directoryStatus.st_uid, ::geteuid());

    const QString socketPath =
        directory.filePath(QStringLiteral("bus"));
    const QByteArray encodedSocketPath =
        QFile::encodeName(socketPath);
    struct sockaddr_un nativeAddress {};
    nativeAddress.sun_family = AF_UNIX;
    QVERIFY(
        encodedSocketPath.size() <
        static_cast<qsizetype>(
            sizeof(nativeAddress.sun_path)));
    std::memcpy(
        nativeAddress.sun_path,
        encodedSocketPath.constData(),
        static_cast<std::size_t>(
            encodedSocketPath.size() + 1));

    ScopedSocketDescriptor socketDescriptor(
        ::socket(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
            0));
    QVERIFY(socketDescriptor.get() >= 0);
    const socklen_t nativeAddressLength =
        static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) +
            encodedSocketPath.size() + 1);
    QCOMPARE(
        ::bind(
            socketDescriptor.get(),
            reinterpret_cast<const struct sockaddr *>(
                &nativeAddress),
            nativeAddressLength),
        0);
    QCOMPARE(::listen(socketDescriptor.get(), 1), 0);

    const QByteArray address =
        QByteArrayLiteral("unix:path=") +
        encodedSocketPath;
    QVERIFY(!tryx::cli::localSessionBusAddressIsSafe(address));

    QProcessEnvironment environment = headlessEnvironment(false);
    environment.insert(
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QString::fromLatin1(address));
    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        environment);

    verifySessionBusUnavailable(result);
    QVERIFY(
        !result.standardOutput.contains(encodedSocketPath));
    QVERIFY(
        !result.standardError.contains(encodedSocketPath));
    errno = 0;
    ScopedSocketDescriptor acceptedDescriptor(
        ::accept4(
            socketDescriptor.get(),
            nullptr,
            nullptr,
            SOCK_CLOEXEC | SOCK_NONBLOCK));
    const int acceptError = errno;
    QCOMPARE(acceptedDescriptor.get(), -1);
    QVERIFY(
        acceptError == EAGAIN ||
        acceptError == EWOULDBLOCK);
    struct stat socketStatus {};
    QCOMPARE(
        ::lstat(encodedSocketPath.constData(), &socketStatus),
        0);
    QVERIFY(S_ISSOCK(socketStatus.st_mode));
}

void TryxCliTests::unsetSessionBusDoesNotAutolaunch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString launcherDirectory =
        directory.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkdir(launcherDirectory));
    const QString launcherPath =
        QDir(launcherDirectory)
            .filePath(QStringLiteral("dbus-launch"));
    const QString markerPath =
        directory.filePath(
            QStringLiteral("autolaunch-called"));

    QFile launcher(launcherPath);
    QVERIFY(launcher.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray script = QByteArrayLiteral(
        "#!/bin/sh\n"
        ": > \"$TRYX_DBUS_LAUNCH_MARKER\"\n"
        "exit 99\n");
    QCOMPARE(launcher.write(script), script.size());
    launcher.close();
    QVERIFY(QFile::setPermissions(
        launcherPath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    QProcessEnvironment environment = headlessEnvironment(false);
    environment.remove(
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"));
    environment.remove(QStringLiteral("XDG_RUNTIME_DIR"));
    environment.insert(
        QStringLiteral("DISPLAY"),
        QStringLiteral(":tryx-cli-autolaunch-test"));
    environment.insert(
        QStringLiteral("PATH"), launcherDirectory);
    environment.insert(
        QStringLiteral("TRYX_DBUS_LAUNCH_MARKER"),
        markerPath);

    QVERIFY(
        !tryx::cli::localSessionBusAddressIsSafe(
            QByteArray()));
    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        environment);

    verifySessionBusUnavailable(result);
    QVERIFY(!QFileInfo::exists(markerPath));
    QVERIFY(
        !result.standardOutput.contains(
            QFile::encodeName(launcherPath)));
    QVERIFY(
        !result.standardError.contains(
            QFile::encodeName(launcherPath)));
}

void TryxCliTests::unsafeOutputDirectoryControlsFailClosed_data() {
    QTest::addColumn<uint>("codePoint");

    for (uint codePoint = 0x80;
         codePoint <= 0x9f;
         ++codePoint) {
        const QByteArray rowName =
            QByteArrayLiteral("c1-") +
            QByteArray::number(codePoint, 16)
                .rightJustified(2, '0');
        QTest::newRow(rowName.constData()) << codePoint;
    }

    QTest::newRow("arabic-letter-mark") << uint{0x061c};
    QTest::newRow("left-to-right-mark") << uint{0x200e};
    QTest::newRow("right-to-left-mark") << uint{0x200f};
    QTest::newRow("line-separator") << uint{0x2028};
    QTest::newRow("paragraph-separator") << uint{0x2029};
}

void TryxCliTests::unsafeOutputDirectoryControlsFailClosed() {
    QFETCH(uint, codePoint);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QChar unsafeCharacter(
        static_cast<char16_t>(codePoint));
    const QString directoryName =
        QStringLiteral("unsafe-") +
        unsafeCharacter +
        QStringLiteral("-directory");
    QVERIFY(QDir(directory.path()).mkdir(directoryName));

    const QString outputDirectory =
        directory.filePath(directoryName);
    QVERIFY(QFile::setPermissions(
        outputDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const ProcessResult result = runCli(
        {
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            outputDirectory,
        },
        headlessEnvironment(false));

    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 11);
    QVERIFY(result.standardOutput.isEmpty());
    QCOMPARE(
        result.standardError,
        QByteArrayLiteral(
            "The output directory is unsafe\n"));
    QVERIFY(
        !result.standardError.contains(
            QFile::encodeName(outputDirectory)));
    QVERIFY(
        !result.standardError.contains(
            QString(unsafeCharacter).toUtf8()));
    QCOMPARE(
        directoryEntries(outputDirectory),
        QStringList());
}

void TryxCliTests::noOwnerStatusReturnsStableJsonError() {
    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 4);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("RUNTIME_UNAVAILABLE"));
}

void TryxCliTests::hostOnlyReportWorksWithoutBus() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            directory.path(),
        },
        headlessEnvironment(true));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("support-report"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.keys(),
        QStringList({
            QStringLiteral("path"),
            QStringLiteral("runtime_snapshot_status"),
        }));
    QCOMPARE(
        data.value(
            QStringLiteral("runtime_snapshot_status")).toString(),
        QStringLiteral("unavailable"));

    const QString reportPath =
        data.value(QStringLiteral("path")).toString();
    QCOMPARE(QFileInfo(reportPath).absolutePath(), directory.path());
    QFile report(reportPath);
    QVERIFY(report.open(QIODevice::ReadOnly));
    const QJsonObject reportRoot =
        QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(
        reportRoot.value(QStringLiteral("schema_version")).toInt(),
        1);
    QCOMPARE(
        reportRoot.value(QStringLiteral("runtime_snapshot"))
            .toObject()
            .value(QStringLiteral("status"))
            .toString(),
        QStringLiteral("unavailable"));
}

void TryxCliTests::hostOnlyReportWorksForUnsupportedRuntime() {
    FakeCliRuntime runtime;
    runtime.apiVersion = tryxRuntimeApiVersion() + 1;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            directory.path(),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("support-report"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.value(QStringLiteral("runtime_snapshot_status"))
            .toString(),
        QStringLiteral("unsupported"));

    QFile report(data.value(QStringLiteral("path")).toString());
    QVERIFY(report.open(QIODevice::ReadOnly));
    const QJsonObject reportRoot =
        QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(
        reportRoot.value(QStringLiteral("runtime_snapshot"))
            .toObject()
            .value(QStringLiteral("status"))
            .toString(),
        QStringLiteral("unsupported"));
    QCOMPARE(runtime.apiCalls.load(), 1);
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 0);
    QCOMPARE(runtime.supportSnapshotCalls.load(), 0);
}

void TryxCliTests::statusUsesValidatedSupportSnapshot() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.keys(),
        QStringList({
            QStringLiteral("counts"),
            QStringLiteral("device"),
            QStringLiteral("generated_at_utc"),
            QStringLiteral("recovery"),
            QStringLiteral("runtime"),
        }));
    const QJsonObject device =
        data.value(QStringLiteral("device")).toObject();
    QCOMPARE(
        device.value(QStringLiteral("product_id")).toString(),
        QStringLiteral("391a:1021"));
    QCOMPARE(
        device.value(QStringLiteral("model")).toString(),
        QStringLiteral("PANORAMA SE"));
    QCOMPARE(
        data.value(QStringLiteral("runtime"))
            .toObject()
            .value(QStringLiteral("api_version"))
            .toInt(),
        8);
    QCOMPARE(runtime.supportSnapshotCalls.load(), 1);
    QCOMPARE(runtime.connectionSnapshotCalls.load(), 0);
}

void TryxCliTests::operationsUseBoundedSupportSnapshotTail() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("operations"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("operations"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.keys(),
        QStringList({
            QStringLiteral("generated_at_utc"),
            QStringLiteral("items"),
            QStringLiteral("operation_count"),
            QStringLiteral("returned_count"),
            QStringLiteral("truncated"),
        }));
    QCOMPARE(
        data.value(QStringLiteral("operation_count")).toInt(),
        47);
    QCOMPARE(
        data.value(QStringLiteral("returned_count")).toInt(),
        32);
    QVERIFY(data.value(QStringLiteral("truncated")).toBool());
    const QJsonArray items =
        data.value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 32);
    QCOMPARE(
        items.first().toObject().keys(),
        QStringList({
            QStringLiteral("apply_after_upload"),
            QStringLiteral("attempt"),
            QStringLiteral("error_category"),
            QStringLiteral("kind"),
            QStringLiteral("primary_error_category"),
            QStringLiteral("retry_mode"),
            QStringLiteral("stage"),
            QStringLiteral("state"),
            QStringLiteral("terminal_outcome"),
        }));
    QCOMPARE(runtime.supportSnapshotCalls.load(), 1);
    QCOMPARE(runtime.connectionSnapshotCalls.load(), 0);
}

void TryxCliTests::
capabilitiesUseCanonicalAllowlistsAndContext() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("capabilities"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("capabilities"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.keys(),
        QStringList({
            QStringLiteral("device_capabilities"),
            QStringLiteral("device_capabilities_status"),
            QStringLiteral("legacy_runtime_capabilities"),
            QStringLiteral("runtime_api_version"),
            QStringLiteral("runtime_capabilities"),
        }));
    QCOMPARE(
        jsonStringList(
            data.value(QStringLiteral("runtime_capabilities"))),
        QStringList({
            tryxRuntimeDeviceCapabilitiesV1Token(),
            tryxRuntimeSupportSnapshotV1Token(),
        }));
    QCOMPARE(
        data.value(
            QStringLiteral("device_capabilities_status"))
            .toString(),
        QStringLiteral("available"));
    QCOMPARE(
        jsonStringList(
            data.value(QStringLiteral("device_capabilities"))),
        QStringList({
            tryxDeviceMediaUploadV1Token(),
            tryxDeviceMediaCatalogV1Token(),
        }));
    QVERIFY(!data.value(
        QStringLiteral("legacy_runtime_capabilities"))
        .toBool());
    QCOMPARE(runtime.connectionSnapshotCalls.load(), 1);
    QCOMPARE(runtime.deviceCapabilityCalls.load(), 1);
    QCOMPARE(runtime.supportSnapshotCalls.load(), 0);
}

void TryxCliTests::
runtimeRequestTargetsExactOwnerWithoutAutostart() {
    const QString owner = QStringLiteral(":1.4242");
    const QString method = QStringLiteral("GetSupportSnapshotV1");
    const QDBusMessage request =
        tryx::cli::runtimeMethodCallForTesting(owner, method);

    QCOMPARE(request.type(), QDBusMessage::MethodCallMessage);
    QCOMPARE(request.service(), owner);
    QCOMPARE(request.path(), tryxRuntimeObjectPath());
    QCOMPARE(
        request.interface(),
        tryxRuntimeOperationsInterfaceName());
    QCOMPARE(request.member(), method);
    QVERIFY(!request.autoStartService());
    QVERIFY(!request.isInteractiveAuthorizationAllowed());
}

void TryxCliTests::legacyUnknownMethodCapabilitiesSucceed() {
    LegacyCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("capabilities"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("capabilities"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.value(QStringLiteral("runtime_api_version")).toInt(),
        8);
    QVERIFY(data.value(
        QStringLiteral("legacy_runtime_capabilities")).toBool());
    QCOMPARE(
        jsonStringList(data.value(
            QStringLiteral("runtime_capabilities"))),
        QStringList());
    QCOMPARE(
        data.value(
            QStringLiteral("device_capabilities_status")).toString(),
        QStringLiteral("unsupported"));
    QCOMPARE(
        jsonStringList(data.value(
            QStringLiteral("device_capabilities"))),
        QStringList());
    QCOMPARE(runtime.apiCalls.load(), 1);
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 1);
}

void TryxCliTests::apiMismatchReturnsStableError() {
    FakeCliRuntime runtime;
    runtime.apiVersion = 7;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 5);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INCOMPATIBLE_RUNTIME"));
    QCOMPARE(runtime.apiCalls.load(), 1);
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 0);
    QCOMPARE(runtime.supportSnapshotCalls.load(), 0);
}

void TryxCliTests::malformedSupportSnapshotFailsWithoutLeak() {
    FakeCliRuntime runtime;
    runtime.supportSnapshot = QStringLiteral(
        "{\"schema_version\":1,\"diagnostic\":\"%1\"}")
                                  .arg(QString::fromLatin1(
                                      kDiagnosticCanary));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 10);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INVALID_REPLY"));
    QCOMPARE(runtime.supportSnapshotCalls.load(), 1);
}

void TryxCliTests::runtimeCapabilityErrorFailsWithoutLeak() {
    FakeCliRuntime runtime;
    runtime.runtimeCapabilityReplyMode =
        FakeCliRuntime::RuntimeCapabilityReplyMode::AccessDenied;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("capabilities"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 9);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("capabilities"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("RUNTIME_CALL_FAILED"));
    QCOMPARE(runtime.runtimeCapabilityCalls.load(), 1);
    QCOMPARE(runtime.connectionSnapshotCalls.load(), 0);
    QCOMPARE(runtime.deviceCapabilityCalls.load(), 0);
}

void TryxCliTests::invalidDeviceContextFailsClosed_data() {
    QTest::addColumn<QString>("deviceIdentity");
    QTest::addColumn<quint64>("connectionRevision");
    QTest::addColumn<quint64>("physicalGeneration");

    QTest::newRow("identity-mismatch")
        << QStringLiteral("different-device")
        << quint64{41} << quint64{73};
    QTest::newRow("revision-mismatch")
        << QString::fromLatin1(kSerialCanary)
        << quint64{42} << quint64{73};
    QTest::newRow("zero-generation")
        << QString::fromLatin1(kSerialCanary)
        << quint64{41} << quint64{0};
}

void TryxCliTests::invalidDeviceContextFailsClosed() {
    QFETCH(QString, deviceIdentity);
    QFETCH(quint64, connectionRevision);
    QFETCH(quint64, physicalGeneration);

    FakeCliRuntime runtime;
    runtime.deviceCapabilities.deviceIdentity = deviceIdentity;
    runtime.deviceCapabilities.connectionRevision = connectionRevision;
    runtime.deviceCapabilities.physicalGeneration = physicalGeneration;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("capabilities"),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 10);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("capabilities"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INVALID_REPLY"));
    QCOMPARE(runtime.connectionSnapshotCalls.load(), 1);
    QCOMPARE(runtime.deviceCapabilityCalls.load(), 1);
    QCOMPARE(runtime.supportSnapshotCalls.load(), 0);
}

void TryxCliTests::
advertisedSupportReportIncludesValidatedRuntimeSnapshot() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    QCOMPARE(directoryEntries(directory.path()), QStringList());

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            directory.path(),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("support-report"), true);
    const QJsonObject data =
        root.value(QStringLiteral("data")).toObject();
    QCOMPARE(
        data.value(
            QStringLiteral("runtime_snapshot_status")).toString(),
        QStringLiteral("available"));

    QFile report(data.value(QStringLiteral("path")).toString());
    QVERIFY(report.open(QIODevice::ReadOnly));
    const QByteArray reportBytes = report.readAll();
    QVERIFY(!reportBytes.contains(kSerialCanary));
    QVERIFY(!reportBytes.contains(kMediaCanary));
    QVERIFY(!reportBytes.contains(kDiagnosticCanary));
    const QJsonObject runtimeSection =
        QJsonDocument::fromJson(reportBytes)
            .object()
            .value(QStringLiteral("runtime_snapshot"))
            .toObject();
    QCOMPARE(
        runtimeSection.value(QStringLiteral("status")).toString(),
        QStringLiteral("available"));
    QCOMPARE(
        runtimeSection.value(QStringLiteral("data")).toObject(),
        QJsonDocument::fromJson(
            runtime.supportSnapshot.toUtf8()).object());
    QCOMPARE(runtime.supportSnapshotCalls.load(), 1);
}

void TryxCliTests::invalidAdvertisedSupportReportCreatesNoFile() {
    FakeCliRuntime runtime;
    runtime.supportSnapshot = QStringLiteral(
        "{\"schema_version\":1,\"diagnostic\":\"%1\"}")
                                  .arg(QString::fromLatin1(
                                      kDiagnosticCanary));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const ProcessResult result = runCli(
        {
            QStringLiteral("--json"),
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            directory.path(),
        },
        headlessEnvironment(false));
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 10);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);

    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("support-report"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("INVALID_REPLY"));
    QCOMPARE(directoryEntries(directory.path()), QStringList());
    QCOMPARE(runtime.supportSnapshotCalls.load(), 1);
}

void TryxCliTests::delayedSnapshotReplyAfterOwnerLossFailsClosed() {
    FakeCliRuntime runtime;
    runtime.supportReplyMode =
        FakeCliRuntime::SupportReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QProcess process;
    configureCliProcess(
        &process,
        {QStringLiteral("--json"), QStringLiteral("status")},
        headlessEnvironment(false));
    process.start();
    QVERIFY2(process.waitForStarted(3000),
             qPrintable(process.errorString()));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.supportSnapshotCalls.load(
            std::memory_order_acquire),
        1, 3000);

    QVERIFY(service.releaseServiceName());
    bool replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &replySent]() {
            replySent = runtime.sendDelayedSupportReply(
                service.connection(), runtime.supportSnapshot);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(
        process.state() == QProcess::NotRunning, 3000);

    const ProcessResult result = collectFinishedCli(&process);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 8);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("OWNER_CHANGED"));
}

void TryxCliTests::shortInternalDeadlineReturnsTimeout() {
    FakeCliRuntime runtime;
    runtime.apiReplyMode = FakeCliRuntime::ApiReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QElapsedTimer elapsed;
    elapsed.start();
    const tryx::cli::RunResult direct = tryx::cli::run(
        {QStringLiteral("--json"), QStringLiteral("status")},
        QDBusConnection::sessionBus(), 50);
    const qint64 elapsedMilliseconds = elapsed.elapsed();
    const ProcessResult result = processResultFromDirectRun(direct);

    QCOMPARE(result.exitCode, 7);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    QVERIFY(elapsedMilliseconds >= 20);
    QVERIFY2(elapsedMilliseconds < 1000,
             qPrintable(QString::number(elapsedMilliseconds)));
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(root, QStringLiteral("status"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("TIMEOUT"));
    QCOMPARE(runtime.apiCalls.load(), 1);
}

void TryxCliTests::
ownerLossBeforeSupportReportPublishCreatesNoFile() {
    FakeCliRuntime runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    bool beforePublishReached = false;
    bool ownerReleased = false;
    tryx::support_bundle::testing::setBeforePublishHook(
        [&service, &beforePublishReached, &ownerReleased](
            const QString &, const QString &) {
            beforePublishReached = true;
            ownerReleased = service.releaseServiceName();
        });
    const tryx::cli::RunResult direct = tryx::cli::run(
        {
            QStringLiteral("--json"),
            QStringLiteral("support-report"),
            QStringLiteral("--output-dir"),
            directory.path(),
        },
        QDBusConnection::sessionBus(), 1000);
    tryx::support_bundle::testing::setBeforePublishHook({});

    QVERIFY(beforePublishReached);
    QVERIFY(ownerReleased);
    const ProcessResult result = processResultFromDirectRun(direct);
    QCOMPARE(result.exitCode, 8);
    QVERIFY(result.standardError.isEmpty());
    verifyNoSensitiveCanaries(result);
    const QJsonObject root =
        parseSingleJsonObject(result.standardOutput);
    QVERIFY(!root.isEmpty());
    verifyEnvelope(
        root, QStringLiteral("support-report"), false);
    QCOMPARE(
        root.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString(),
        QStringLiteral("OWNER_CHANGED"));
    QCOMPARE(directoryEntries(directory.path()), QStringList());
}

QTEST_GUILESS_MAIN(TryxCliTests)

#include "cli_tests.moc"
