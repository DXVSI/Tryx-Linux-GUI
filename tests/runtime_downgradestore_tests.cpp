#include "runtimedowngradestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using Store = tryx::RuntimeDowngradeStore;

Store::ExecutableIdentity identity(
    quint64 inode = 22,
    const QByteArray &digest = QByteArray(32, '\x11')) {
    Store::ExecutableIdentity value;
    value.path = QStringLiteral("/usr/lib/tryx-panorama-manager/tryx-panorama-runtime");
    value.device = 11;
    value.inode = inode;
    value.size = 4096;
    value.sha256 = digest;
    return value;
}

bool writeFile(const QString &path, const QByteArray &payload,
               QFileDevice::Permissions permissions) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(payload) != payload.size()) {
        return false;
    }
    file.close();
    return QFile::setPermissions(path, permissions);
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QJsonObject markerObject(const QString &mode = QStringLiteral("Empty"),
                         quint64 revision = 7) {
    const Store::ExecutableIdentity executable = identity();
    QJsonObject object;
    object.insert(QStringLiteral("schema"), Store::SchemaVersion);
    object.insert(QStringLiteral("targetRetryFormat"),
                  Store::TargetRetryFormat);
    object.insert(QStringLiteral("mode"), mode);
    object.insert(QStringLiteral("storeRevision"),
                  QString::number(revision));
    object.insert(QStringLiteral("executablePath"), executable.path);
    object.insert(QStringLiteral("executableDevice"),
                  QString::number(executable.device));
    object.insert(QStringLiteral("executableInode"),
                  QString::number(executable.inode));
    object.insert(QStringLiteral("executableSize"),
                  QString::number(executable.size));
    object.insert(QStringLiteral("executableSha256"),
                  QString::fromLatin1(executable.sha256.toHex()));
    return object;
}

QByteArray markerBytes(const QString &mode = QStringLiteral("Empty"),
                       quint64 revision = 7) {
    return QJsonDocument(markerObject(mode, revision))
        .toJson(QJsonDocument::Compact);
}

Store::ExecutableIdentity executableIdentityForPath(
    const QString &path) {
    Store::ExecutableIdentity identity;
    const QString absolutePath =
        QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    struct stat status {};
    if (::lstat(QFile::encodeName(absolutePath).constData(),
                &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0) {
        return {};
    }
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(chunk);
    }
    identity.path = absolutePath;
    identity.device = static_cast<quint64>(status.st_dev);
    identity.inode = static_cast<quint64>(status.st_ino);
    identity.size = static_cast<quint64>(status.st_size);
    identity.sha256 = hash.result();
    return identity;
}

struct RuntimeProcessResult {
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    QString processError;
};

RuntimeProcessResult runRuntimeWithState(const QString &stateRoot) {
    QProcess process;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_STATE_HOME"), stateRoot);
    environment.insert(
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("unix:path=%1")
            .arg(QDir(stateRoot).filePath(
                QStringLiteral("missing-session-bus"))));
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral(TRYX_RUNTIME_BINARY));
    process.start();

    RuntimeProcessResult result;
    result.started = process.waitForStarted(5000);
    if (!result.started) {
        result.processError = process.errorString();
        return result;
    }
    result.finished = process.waitForFinished(10000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(3000);
        result.processError = QStringLiteral(
            "Runtime process did not finish within its bounded deadline");
    }
    result.exitStatus = process.exitStatus();
    result.exitCode = process.exitCode();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

QString processStoreDirectory(const QString &stateRoot) {
    return QDir(stateRoot).filePath(
        QStringLiteral(
            "tryx-panorama-manager-downgrade-v10"));
}

}  // namespace

Q_DECLARE_METATYPE(
    tryx::RuntimeDowngradeStore::ExecutableIdentity)

class RuntimeDowngradeStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void defaultDirectoryUsesGenericStateLocation();
    void currentExecutableIdentityIsComplete();
    void runtimeStartupGateBlocksExactExecutableBeforeDbus();
    void runtimeStartupGateIgnoresDifferentExecutable();
    void persistCreatesOwnerOnlyDurableMarker();
    void fullFrameRoundTripIsExact();
    void differentExecutableDoesNotBlockOrAbort();
    void abortRemovesOnlyExactCurrentExecutableMarker();
    void offlineAbortHashesTheInstalledMarkerExecutable();
    void offlineAbortRejectsDifferentOrSymlinkExecutable();
    void persistIsIdempotentButCannotChangeCurrentIntent();
    void postRenameFailureReportsCommittedUnknownOutcome();
    void aNewExecutableCanReplaceAValidStaleMarker();
    void invalidInputDoesNotCreateMarker_data();
    void invalidInputDoesNotCreateMarker();
    void corruptMarkersFailClosed_data();
    void corruptMarkersFailClosed();
    void unsafeDirectoryAndMarkerFailClosed();
    void abortRefusesCorruptOrUnsafeMarker();
};

void RuntimeDowngradeStoreTests::
    defaultDirectoryUsesGenericStateLocation() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QString state = QDir::cleanPath(
        QStandardPaths::writableLocation(
            QStandardPaths::GenericStateLocation));
#else
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty() || !QDir::isAbsolutePath(state)) {
        state = QDir(QDir::homePath()).filePath(
            QStringLiteral(".local/state"));
    }
    state = QDir::cleanPath(state);
#endif
    const QString directory = Store::defaultDirectory();
    QVERIFY(!state.isEmpty());
    QVERIFY(QFileInfo(directory).isAbsolute());
    QVERIFY(directory.startsWith(state + QLatin1Char('/')));
    QVERIFY(!directory.contains(QStringLiteral("media-spool")));
    QVERIFY(!directory.contains(QStringLiteral("media-inbox")));
}

void RuntimeDowngradeStoreTests::currentExecutableIdentityIsComplete() {
    QString detail;
    const Store::ExecutableIdentity executable =
        Store::currentExecutableIdentity(&detail);
    QVERIFY2(executable.isValid(), qPrintable(detail));
    QVERIFY(QFileInfo(executable.path).isAbsolute());
    QVERIFY(executable.size > 0);
    QCOMPARE(executable.sha256.size(), 32);
}

void RuntimeDowngradeStoreTests::
    runtimeStartupGateBlocksExactExecutableBeforeDbus() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const Store::ExecutableIdentity runtimeIdentity =
        executableIdentityForPath(
            QStringLiteral(TRYX_RUNTIME_BINARY));
    QVERIFY(runtimeIdentity.isValid());
    Store store(processStoreDirectory(state.path()));
    const Store::PersistResult persisted = store.persist(
        QStringLiteral("Empty"), 31, runtimeIdentity);
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));

    const RuntimeProcessResult result =
        runRuntimeWithState(state.path());
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.standardOutput.isEmpty());
    QVERIFY(QFileInfo::exists(store.markerPath()));
}

void RuntimeDowngradeStoreTests::
    runtimeStartupGateIgnoresDifferentExecutable() {
    QTemporaryDir state;
    QVERIFY(state.isValid());
    Store::ExecutableIdentity differentIdentity =
        executableIdentityForPath(
            QStringLiteral(TRYX_RUNTIME_BINARY));
    QVERIFY(differentIdentity.isValid());
    differentIdentity.sha256[0] =
        static_cast<char>(differentIdentity.sha256.at(0) ^ 0x01);
    Store store(processStoreDirectory(state.path()));
    const Store::PersistResult persisted = store.persist(
        QStringLiteral("Empty"), 32, differentIdentity);
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));

    const RuntimeProcessResult result =
        runRuntimeWithState(state.path());
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(result.finished, qPrintable(result.processError));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 2);
    QVERIFY(result.standardOutput.isEmpty());
    QVERIFY(QFileInfo::exists(store.markerPath()));
}

void RuntimeDowngradeStoreTests::
    persistCreatesOwnerOnlyDurableMarker() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString directory =
        root.filePath(QStringLiteral("downgrade-store"));
    Store store(directory);

    const Store::PersistResult persisted =
        store.persist(QStringLiteral("Empty"), 7, identity());
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));
    QVERIFY(persisted.commitMayExist);

    struct stat directoryStatus {};
    QVERIFY(::lstat(QFile::encodeName(directory).constData(),
                    &directoryStatus) == 0);
    QVERIFY(S_ISDIR(directoryStatus.st_mode));
    QCOMPARE(directoryStatus.st_uid, ::geteuid());
    QCOMPARE(directoryStatus.st_mode & 07777,
             static_cast<mode_t>(S_IRWXU));

    struct stat markerStatus {};
    QVERIFY(::lstat(QFile::encodeName(store.markerPath()).constData(),
                    &markerStatus) == 0);
    QVERIFY(S_ISREG(markerStatus.st_mode));
    QCOMPARE(markerStatus.st_uid, ::geteuid());
    QCOMPARE(markerStatus.st_mode & 07777,
             static_cast<mode_t>(S_IRUSR | S_IWUSR));
    QCOMPARE(markerStatus.st_nlink, static_cast<nlink_t>(1));

    const QJsonDocument document =
        QJsonDocument::fromJson(readFile(store.markerPath()));
    QVERIFY(document.isObject());
    const QJsonObject object = document.object();
    QCOMPARE(object.value(QStringLiteral("schema")).toInteger(),
             qint64{Store::SchemaVersion});
    QCOMPARE(object.value(
                 QStringLiteral("targetRetryFormat")).toInteger(),
             qint64{Store::TargetRetryFormat});
    QCOMPARE(object.value(QStringLiteral("storeRevision")).toString(),
             QStringLiteral("7"));

    const Store::InspectResult inspected = store.inspect(identity());
    QCOMPARE(inspected.status,
             Store::InspectStatus::BlockedCurrentExecutable);
    QCOMPARE(inspected.mode, QStringLiteral("Empty"));
    QCOMPARE(inspected.storeRevision, quint64{7});
}

void RuntimeDowngradeStoreTests::fullFrameRoundTripIsExact() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    const Store::PersistResult persisted =
        store.persist(QStringLiteral("FullFrame"), 18446744073709551615ULL,
                      identity());
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));

    const Store::InspectResult inspected = store.inspect(identity());
    QCOMPARE(inspected.status,
             Store::InspectStatus::BlockedCurrentExecutable);
    QCOMPARE(inspected.mode, QStringLiteral("FullFrame"));
    QCOMPARE(inspected.storeRevision,
             quint64{18446744073709551615ULL});
}

void RuntimeDowngradeStoreTests::
    differentExecutableDoesNotBlockOrAbort() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    QVERIFY(store.persist(QStringLiteral("Empty"), 1, identity()).ok);

    const Store::ExecutableIdentity replacement = identity(23);
    const Store::InspectResult inspected = store.inspect(replacement);
    QCOMPARE(inspected.status,
             Store::InspectStatus::DifferentExecutable);
    QCOMPARE(inspected.mode, QStringLiteral("Empty"));
    QCOMPARE(inspected.storeRevision, quint64{1});

    const Store::AbortResult aborted =
        store.abortForExecutable(replacement);
    QCOMPARE(aborted.status, Store::AbortStatus::DifferentExecutable);
    QVERIFY(QFileInfo::exists(store.markerPath()));
}

void RuntimeDowngradeStoreTests::
    abortRemovesOnlyExactCurrentExecutableMarker() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    QVERIFY(store.persist(QStringLiteral("Empty"), 1, identity()).ok);

    const Store::AbortResult aborted =
        store.abortForExecutable(identity());
    QCOMPARE(aborted.status, Store::AbortStatus::Aborted);
    QVERIFY(!QFileInfo::exists(store.markerPath()));
    QCOMPARE(store.inspect(identity()).status,
             Store::InspectStatus::Missing);
    QCOMPARE(store.abortForExecutable(identity()).status,
             Store::AbortStatus::Missing);
}

void RuntimeDowngradeStoreTests::
    offlineAbortHashesTheInstalledMarkerExecutable() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    QString detail;
    const Store::ExecutableIdentity current =
        Store::currentExecutableIdentity(&detail);
    QVERIFY2(current.isValid(), qPrintable(detail));
    QVERIFY(store.persist(QStringLiteral("Empty"), 1, current).ok);

    const Store::AbortResult aborted =
        store.abortForInstalledExecutable();
    QCOMPARE(aborted.status, Store::AbortStatus::Aborted);
    QVERIFY(!QFileInfo::exists(store.markerPath()));
}

void RuntimeDowngradeStoreTests::
    offlineAbortRejectsDifferentOrSymlinkExecutable() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString executablePath =
        root.filePath(QStringLiteral("runtime-binary"));
    QVERIFY(writeFile(executablePath, QByteArrayLiteral("runtime"),
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner |
                          QFileDevice::ExeOwner));

    Store::ExecutableIdentity stale = identity();
    stale.path = executablePath;
    Store differentStore(
        root.filePath(QStringLiteral("different-store")));
    QVERIFY(differentStore.persist(
        QStringLiteral("Empty"), 1, stale).ok);
    QCOMPARE(differentStore.abortForInstalledExecutable().status,
             Store::AbortStatus::DifferentExecutable);
    QVERIFY(QFileInfo::exists(differentStore.markerPath()));

    const QString symlinkPath =
        root.filePath(QStringLiteral("runtime-link"));
    QVERIFY(QFile::link(executablePath, symlinkPath));
    Store::ExecutableIdentity linked = identity();
    linked.path = symlinkPath;
    Store symlinkStore(root.filePath(QStringLiteral("symlink-store")));
    QVERIFY(symlinkStore.persist(
        QStringLiteral("Empty"), 1, linked).ok);
    QCOMPARE(symlinkStore.abortForInstalledExecutable().status,
             Store::AbortStatus::Unsafe);
    QVERIFY(QFileInfo::exists(symlinkStore.markerPath()));
}

void RuntimeDowngradeStoreTests::
    persistIsIdempotentButCannotChangeCurrentIntent() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    QVERIFY(store.persist(QStringLiteral("Empty"), 4, identity()).ok);
    const QByteArray committed = readFile(store.markerPath());

    const Store::PersistResult repeated =
        store.persist(QStringLiteral("Empty"), 4, identity());
    QVERIFY2(repeated.ok, qPrintable(repeated.detail));
    QCOMPARE(readFile(store.markerPath()), committed);

    const Store::PersistResult changed =
        store.persist(QStringLiteral("FullFrame"), 5, identity());
    QVERIFY(!changed.ok);
    QVERIFY(changed.commitMayExist);
    QVERIFY(!changed.detail.isEmpty());
    QCOMPARE(readFile(store.markerPath()), committed);
}

void RuntimeDowngradeStoreTests::
    postRenameFailureReportsCommittedUnknownOutcome() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    store.setPostRenameVerificationFailureForTesting(true);

    const Store::PersistResult persisted =
        store.persist(QStringLiteral("Empty"), 12, identity());
    QVERIFY(!persisted.ok);
    QVERIFY(persisted.commitMayExist);
    QVERIFY(!persisted.detail.isEmpty());
    QVERIFY(QFileInfo::exists(store.markerPath()));

    const Store::InspectResult inspected = store.inspect(identity());
    QCOMPARE(inspected.status,
             Store::InspectStatus::BlockedCurrentExecutable);
    QCOMPARE(inspected.mode, QStringLiteral("Empty"));
    QCOMPARE(inspected.storeRevision, quint64{12});
}

void RuntimeDowngradeStoreTests::
    aNewExecutableCanReplaceAValidStaleMarker() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));
    QVERIFY(store.persist(QStringLiteral("Empty"), 4, identity()).ok);

    const Store::ExecutableIdentity replacement =
        identity(23, QByteArray(32, '\x22'));
    const Store::PersistResult persisted =
        store.persist(QStringLiteral("FullFrame"), 9, replacement);
    QVERIFY2(persisted.ok, qPrintable(persisted.detail));
    const Store::InspectResult inspected = store.inspect(replacement);
    QCOMPARE(inspected.status,
             Store::InspectStatus::BlockedCurrentExecutable);
    QCOMPARE(inspected.mode, QStringLiteral("FullFrame"));
    QCOMPARE(inspected.storeRevision, quint64{9});
}

void RuntimeDowngradeStoreTests::invalidInputDoesNotCreateMarker_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<Store::ExecutableIdentity>("identity");

    QTest::newRow("unsupported-mode")
        << QStringLiteral("Full") << identity();
    Store::ExecutableIdentity relative = identity();
    relative.path = QStringLiteral("tryx-panorama-runtime");
    QTest::newRow("relative-path")
        << QStringLiteral("Empty") << relative;
    Store::ExecutableIdentity missingHash = identity();
    missingHash.sha256.clear();
    QTest::newRow("missing-hash")
        << QStringLiteral("Empty") << missingHash;
    Store::ExecutableIdentity emptyExecutable = identity();
    emptyExecutable.size = 0;
    QTest::newRow("empty-executable")
        << QStringLiteral("Empty") << emptyExecutable;
}

void RuntimeDowngradeStoreTests::invalidInputDoesNotCreateMarker() {
    QFETCH(QString, mode);
    QFETCH(Store::ExecutableIdentity, identity);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Store store(root.filePath(QStringLiteral("downgrade-store")));

    const Store::PersistResult persisted =
        store.persist(mode, 1, identity);
    QVERIFY(!persisted.ok);
    QVERIFY(!persisted.commitMayExist);
    QVERIFY(!persisted.detail.isEmpty());
    QVERIFY(!QFileInfo::exists(store.markerPath()));
}

void RuntimeDowngradeStoreTests::corruptMarkersFailClosed_data() {
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("malformed") << QByteArrayLiteral("{not-json");
    QJsonObject object = markerObject();
    object.insert(QStringLiteral("schema"), 2);
    QTest::newRow("wrong-schema")
        << QJsonDocument(object).toJson(QJsonDocument::Compact);
    object = markerObject();
    object.insert(QStringLiteral("targetRetryFormat"), 11);
    QTest::newRow("wrong-target")
        << QJsonDocument(object).toJson(QJsonDocument::Compact);
    object = markerObject();
    object.insert(QStringLiteral("schema"), 1.5);
    QTest::newRow("fractional-schema")
        << QJsonDocument(object).toJson(QJsonDocument::Compact);
    object = markerObject();
    object.insert(QStringLiteral("storeRevision"),
                  QStringLiteral("01"));
    QTest::newRow("noncanonical-revision")
        << QJsonDocument(object).toJson(QJsonDocument::Compact);
    object = markerObject();
    object.insert(QStringLiteral("unexpected"), true);
    QTest::newRow("extra-field")
        << QJsonDocument(object).toJson(QJsonDocument::Compact);
    QTest::newRow("duplicate-key")
        << QByteArrayLiteral(
               "{\"schema\":1,\"schema\":1,\"targetRetryFormat\":10,"
               "\"mode\":\"Empty\",\"storeRevision\":\"7\","
               "\"executablePath\":\"/usr/lib/tryx-panorama-manager/tryx-panorama-runtime\","
               "\"executableDevice\":\"11\",\"executableInode\":\"22\","
               "\"executableSize\":\"4096\","
               "\"executableSha256\":\"1111111111111111111111111111111111111111111111111111111111111111\"}");
}

void RuntimeDowngradeStoreTests::corruptMarkersFailClosed() {
    QFETCH(QByteArray, payload);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString directory =
        root.filePath(QStringLiteral("downgrade-store"));
    QVERIFY(QDir().mkdir(directory));
    QVERIFY(QFile::setPermissions(directory,
                                  QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));
    Store store(directory);
    QVERIFY(writeFile(store.markerPath(), payload,
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner));

    const Store::InspectResult inspected = store.inspect(identity());
    QCOMPARE(inspected.status, Store::InspectStatus::Corrupt);
    QVERIFY(!inspected.detail.isEmpty());
    const Store::PersistResult persisted =
        store.persist(QStringLiteral("Empty"), 8, identity());
    QVERIFY(!persisted.ok);
    QCOMPARE(readFile(store.markerPath()), payload);
}

void RuntimeDowngradeStoreTests::unsafeDirectoryAndMarkerFailClosed() {
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const QString unsafeDirectory =
        root.filePath(QStringLiteral("unsafe-directory"));
    QVERIFY(QDir().mkdir(unsafeDirectory));
    QVERIFY(QFile::setPermissions(
        unsafeDirectory,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup));
    Store unsafeDirectoryStore(unsafeDirectory);
    QVERIFY(writeFile(unsafeDirectoryStore.markerPath(), markerBytes(),
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner));
    QCOMPARE(unsafeDirectoryStore.inspect(identity()).status,
             Store::InspectStatus::Unsafe);

    const QString directory =
        root.filePath(QStringLiteral("unsafe-marker"));
    QVERIFY(QDir().mkdir(directory));
    QVERIFY(QFile::setPermissions(directory,
                                  QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));
    Store unsafeMarkerStore(directory);
    QVERIFY(writeFile(unsafeMarkerStore.markerPath(), markerBytes(),
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner |
                          QFileDevice::ReadGroup));
    QCOMPARE(unsafeMarkerStore.inspect(identity()).status,
             Store::InspectStatus::Unsafe);

    QVERIFY(QFile::remove(unsafeMarkerStore.markerPath()));
    const QString target = root.filePath(QStringLiteral("target"));
    QVERIFY(writeFile(target, markerBytes(),
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner));
    QVERIFY(QFile::link(target, unsafeMarkerStore.markerPath()));
    QCOMPARE(unsafeMarkerStore.inspect(identity()).status,
             Store::InspectStatus::Unsafe);
}

void RuntimeDowngradeStoreTests::abortRefusesCorruptOrUnsafeMarker() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString directory =
        root.filePath(QStringLiteral("downgrade-store"));
    QVERIFY(QDir().mkdir(directory));
    QVERIFY(QFile::setPermissions(directory,
                                  QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));
    Store store(directory);

    QVERIFY(writeFile(store.markerPath(), QByteArrayLiteral("{}"),
                      QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner));
    QCOMPARE(store.abortForExecutable(identity()).status,
             Store::AbortStatus::Corrupt);
    QVERIFY(QFileInfo::exists(store.markerPath()));

    QVERIFY(QFile::setPermissions(store.markerPath(),
                                  QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup));
    QCOMPARE(store.abortForExecutable(identity()).status,
             Store::AbortStatus::Unsafe);
    QVERIFY(QFileInfo::exists(store.markerPath()));
}

QTEST_APPLESS_MAIN(RuntimeDowngradeStoreTests)

#include "runtime_downgradestore_tests.moc"
