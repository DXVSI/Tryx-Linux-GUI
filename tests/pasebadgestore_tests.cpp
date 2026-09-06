#include "pasemetricsconfigstore.h"
#include "configurationformatbackup.h"

#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <fcntl.h>
#include <unistd.h>

namespace {
using Store = tryx::PaseMetricsConfigStore;
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
PrinterProtocol::PaseOverlayConfig customOverlay() {
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.dualMode = true;
    overlay.left.badges = {QStringLiteral("CPU Badge")};
    overlay.right.badges = overlay.left.badges;
    overlay.badgeChoices.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("private one")};
    overlay.badgeChoices.secondaryCpu = {QStringLiteral("Custom"), QStringLiteral("Вторая сторона")};
    return overlay;
}
}

class PaseBadgeStoreTests final : public QObject {
    Q_OBJECT
private slots:
    void roundTrip();
    void legacyReadDoesNotWriteAndUpgradeHasBackup();
    void malformedFailsClosed_data();
    void malformedFailsClosed();
    void backupRejectsUnsafeState_data();
    void backupRejectsUnsafeState();
    void backupRetryRequiresDurableDirectory();
    void backupRemainsBoundToOpenedDirectory();
};

void PaseBadgeStoreTests::backupRemainsBoundToOpenedDirectory() {
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString original = parent.filePath(QStringLiteral("original"));
    const QString moved = parent.filePath(QStringLiteral("moved"));
    QVERIFY(QDir().mkpath(original));
    const QByteArray bytes = QByteArrayLiteral("{\"version\":1}");
    QVERIFY(writeFile(QDir(original).filePath(QStringLiteral("index.json")), bytes));
    const int descriptor = ::open(QFile::encodeName(original).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(descriptor >= 0);
    QVERIFY(QDir().rename(original, moved));
    QVERIFY(QDir().mkpath(original));
    QString error;
    const bool ok = tryx::preserveConfigurationBeforeUpgradeAt(descriptor, QStringLiteral("index.json"), 1024, 2, {1}, &error);
    ::close(descriptor);
    QVERIFY2(ok, qPrintable(error));
    const QString backup = QStringLiteral("index.json.pre-c16-")
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QCOMPARE(readFile(QDir(moved).filePath(backup)), bytes);
    QVERIFY(QDir(original).entryList(QDir::Files).isEmpty());
}

void PaseBadgeStoreTests::roundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Store store(dir.path());
    auto overlay = customOverlay();
    overlay.cpuBadgeText = QStringLiteral("transient-auto-model");
    QVERIFY(store.persist(QStringLiteral("device"), overlay).ok());
    const auto bytes = readFile(store.configPath());
    QVERIFY(!bytes.contains("transient-auto-model"));
    const auto root = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 3);
    Store loaded(dir.path());
    QCOMPARE(loaded.load().status, Store::LoadStatus::Loaded);
    const auto restored = loaded.overlayForDevice(QStringLiteral("device"));
    QVERIFY(restored.has_value());
    QCOMPARE(restored->badgeChoices, overlay.badgeChoices);
    QVERIFY(!loaded.overlayForDevice(QStringLiteral("other")));
    overlay.badgeChoices.primaryCpu.text = QStringLiteral("private\ninvalid");
    QVERIFY(!loaded.persist(QStringLiteral("device"), overlay).ok());
    QCOMPARE(readFile(store.configPath()), bytes);
}

void PaseBadgeStoreTests::legacyReadDoesNotWriteAndUpgradeHasBackup() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Store store(dir.path());
    auto overlay = customOverlay();
    overlay.badgeChoices = {};
    QVERIFY(store.persist(QStringLiteral("device"), overlay).ok());
    auto root = QJsonDocument::fromJson(readFile(store.configPath())).object();
    root.insert(QStringLiteral("version"), 2);
    root.remove(QStringLiteral("badgeChoices"));
    const auto legacy = QJsonDocument(root).toJson();
    QVERIFY(writeFile(store.configPath(), legacy));
    Store loaded(dir.path());
    QCOMPARE(loaded.load().status, Store::LoadStatus::Loaded);
    QCOMPARE(loaded.overlayForDevice(QStringLiteral("device"))->badgeChoices, TryxRuntimeOverlayBadgesV1());
    QCOMPARE(readFile(store.configPath()), legacy);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
    QVERIFY(loaded.persist(QStringLiteral("device"), customOverlay()).ok());
    const auto backups = QDir(dir.path()).entryList({QStringLiteral("pase-metrics.json.pre-c16-*")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    QCOMPARE(readFile(QDir(dir.path()).filePath(backups.first())), legacy);
    QCOMPARE(QFileInfo(QDir(dir.path()).filePath(backups.first())).permissions()
                 & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther),
             QFileDevice::Permissions());
}

void PaseBadgeStoreTests::malformedFailsClosed_data() {
    QTest::addColumn<QString>("caseName");
    for (const char *name : {"missing", "future", "fractional-version", "future-badges", "hidden-custom", "invalid-text", "extra"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PaseBadgeStoreTests::malformedFailsClosed() {
    QFETCH(QString, caseName);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Store store(dir.path());
    QVERIFY(store.persist(QStringLiteral("device"), customOverlay()).ok());
    auto root = QJsonDocument::fromJson(readFile(store.configPath())).object();
    root.insert(QStringLiteral("version"), 3);
    auto choices = tryxOverlayBadgesV1ToJson(customOverlay().badgeChoices);
    if (caseName == QStringLiteral("future")) root.insert(QStringLiteral("version"), 4);
    if (caseName == QStringLiteral("fractional-version")) root.insert(QStringLiteral("version"), 3.5);
    if (caseName == QStringLiteral("future-badges")) choices.insert(QStringLiteral("schemaVersion"), 2);
    if (caseName == QStringLiteral("invalid-text")) choices.insert(QStringLiteral("primaryCpu"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("Custom")}, {QStringLiteral("text"), QStringLiteral("private\ntext")}});
    if (caseName == QStringLiteral("hidden-custom")) root.insert(QStringLiteral("dualMode"), false);
    root.insert(QStringLiteral("badgeChoices"), choices);
    if (caseName == QStringLiteral("missing")) root.remove(QStringLiteral("badgeChoices"));
    if (caseName == QStringLiteral("extra")) root.insert(QStringLiteral("unknown"), true);
    const auto bytes = QJsonDocument(root).toJson();
    QVERIFY(writeFile(store.configPath(), bytes));
    Store loaded(dir.path());
    const auto result = loaded.load();
    QVERIFY(!result.writesEnabled);
    QVERIFY(!loaded.overlayForDevice(QStringLiteral("device")));
    QVERIFY(!loaded.persist(QStringLiteral("device"), customOverlay()).ok());
    QVERIFY(!result.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("private")));
    QCOMPARE(readFile(store.configPath()), bytes);
}

void PaseBadgeStoreTests::backupRejectsUnsafeState_data() {
    QTest::addColumn<QString>("kind");
    for (const char *name : {"future", "malformed", "oversized", "source-symlink", "backup-symlink", "backup-changed", "backup-readable"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PaseBadgeStoreTests::backupRejectsUnsafeState() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    const QString external = dir.filePath(QStringLiteral("private-other.json"));
    const QByteArray legacy("{\"version\":2}");
    QByteArray bytes = legacy;
    if (kind == QStringLiteral("future")) bytes = "{\"version\":4}";
    if (kind == QStringLiteral("malformed")) bytes = "private malformed";
    if (kind == QStringLiteral("oversized")) bytes = QByteArray(2048, ' ');
    QVERIFY(writeFile(external, QByteArray("private external")));
    if (kind == QStringLiteral("source-symlink")) QVERIFY(QFile::link(external, path));
    else QVERIFY(writeFile(path, bytes));
    const QString backup = path + QStringLiteral(".pre-c16-")
        + QString::fromLatin1(QCryptographicHash::hash(legacy, QCryptographicHash::Sha256).toHex());
    if (kind == QStringLiteral("backup-symlink")) QVERIFY(QFile::link(external, backup));
    if (kind == QStringLiteral("backup-changed")) QVERIFY(writeFile(backup, QByteArray("private changed")));
    if (kind == QStringLiteral("backup-readable")) {
        QVERIFY(writeFile(backup, legacy));
        QVERIFY(QFile::setPermissions(backup, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
    }
    const QByteArray before = readFile(path);
    QString error;
    QVERIFY(!tryx::preserveConfigurationBeforeUpgrade(path, 1024, 3, {1, 2}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(QStringLiteral("private")));
    QCOMPARE(readFile(path), before);
    QCOMPARE(readFile(external), QByteArray("private external"));
}

void PaseBadgeStoreTests::backupRetryRequiresDurableDirectory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    const QByteArray legacy("{\"version\":2}");
    QVERIFY(writeFile(path, legacy));
    QString error;
    tryx::setConfigurationBackupDirectorySyncFailureForTesting(true);
    const bool initial = tryx::preserveConfigurationBeforeUpgrade(path, 1024, 3, {1, 2}, &error);
    const bool retry = tryx::preserveConfigurationBeforeUpgrade(path, 1024, 3, {1, 2}, &error);
    tryx::setConfigurationBackupDirectorySyncFailureForTesting(false);
    QVERIFY(!initial);
    QVERIFY(!retry);
    QCOMPARE(readFile(path), legacy);
    QVERIFY(tryx::preserveConfigurationBeforeUpgrade(path, 1024, 3, {1, 2}, &error));
}

QTEST_GUILESS_MAIN(PaseBadgeStoreTests)
#include "pasebadgestore_tests.moc"
