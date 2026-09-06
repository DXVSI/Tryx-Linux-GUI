#include "savedlayoutstore.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using Store = tryx::SavedLayoutStore;

constexpr auto kDeviceA = "PASE-DEVICE-A";
constexpr auto kDeviceB = "PASE-DEVICE-B";
constexpr auto kProduct = "391a:1021";

bool writeFile(const QString &path, const QByteArray &payload,
               QFileDevice::Permissions permissions =
                   QFileDevice::ReadOwner |
                   QFileDevice::WriteOwner) {
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

TryxRuntimeSavedMediaRefV1 mediaRef(
    const QString &deviceIdentity, const QString &name, quint64 size,
    quint32 source = 1U, bool readOnly = false) {
    TryxRuntimeSavedMediaRefV1 ref;
    ref.name = name;
    ref.size = size;
    ref.source = source;
    ref.readOnly = readOnly;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    hash.addData(deviceIdentity.toUtf8());
    hash.addData(separator);
    hash.addData(name.toUtf8());
    hash.addData(separator);
    hash.addData(QByteArray::number(size));
    hash.addData(separator);
    hash.addData(QByteArray::number(source));
    ref.mediaId = QString::fromLatin1(hash.result().toHex());
    return ref;
}

TryxRuntimeSavedLayoutV1 layout(
    const QString &id,
    const QString &device = QString::fromLatin1(kDeviceA),
    const QString &name = QStringLiteral("Gaming"),
    bool split = false) {
    TryxRuntimeSavedLayoutV1 value;
    value.layoutId = id;
    value.deviceIdentity = device;
    value.productId = QString::fromLatin1(kProduct);
    value.name = name;
    value.media = {
        mediaRef(device,
                 QStringLiteral("game.h264_2240x1080"), 1024)};
    value.request.media = {value.media.constFirst().name};
    value.request.ratio = QStringLiteral("2:1");
    value.request.screenMode = QStringLiteral("Full Screen");
    value.request.playMode = QStringLiteral("Loop");
    value.request.sysinfoLabels = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("GPU Temperature")};
    value.request.settingsPosition = QStringLiteral("Top");
    value.request.settingsColor = QStringLiteral("#112233");
    value.request.settingsAlign = QStringLiteral("Center");
    value.request.settingsBadges = {QStringLiteral("CPU Badge")};
    value.request.replaceOverlay = true;
    value.request.display.brightnessPresent = true;
    value.request.display.brightness = 67;
    value.request.display.orientationPresent = true;
    value.request.display.mirrorMode = false;
    value.request.display.waterfallMode = true;
    value.request.waterfallMode = true;
    if (split) {
        value.media.append(
            mediaRef(device,
                     QStringLiteral("chat.h264_2240x1080"), 2048,
                     2U, true));
        value.request.media.append(value.media.constLast().name);
        value.request.screenMode = QStringLiteral("Screen Splitting");
        value.request.playMode = QStringLiteral("Single");
        value.request.sysinfoLabels2 = {
            QStringLiteral("GPU Usage")};
        value.request.settingsBadges2 = {
            QStringLiteral("GPU Badge")};
        value.request.settingsPosition2 = QStringLiteral("Bottom");
        value.request.settingsColor2 = QStringLiteral("#445566");
        value.request.settingsAlign2 = QStringLiteral("Right");
    }
    return value;
}

QString id(int suffix) {
    return QStringLiteral("11111111-1111-4111-8111-%1")
        .arg(suffix, 12, 10, QLatin1Char('0'));
}

}  // namespace

class SavedLayoutStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void versionedRoundTripPreservesExactDeviceScopedDraft();
    void otherDeviceLayoutsAreFilteredButNotDeleted();
    void snapshotRejectsNonCanonicalDeviceIdentity_data();
    void snapshotRejectsNonCanonicalDeviceIdentity();
    void deleteRequiresExactDeviceAndProductScope();
    void putUsesSnapshotCasAndCaseInsensitiveNames();
    void updateAndDeleteAreAtomicAcrossRestart();
    void recordRevisionAndDeviceScopeAreImmutable();
    void limitsAndCanonicalSerializationAreEnforced();
    void invalidLayoutNeverMutatesStore_data();
    void invalidLayoutNeverMutatesStore();
    void malformedFutureOversizedAndUnsafeStateFailClosed_data();
    void malformedFutureOversizedAndUnsafeStateFailClosed();
    void postCommitDirectoryRaceReportsUnknownAndDisablesWrites();
    void customBadgesRoundTripAndLegacyCannotEraseThem();
    void legacyUpgradePreservesBackup();
    void malformedBadgeRecordsFailClosed_data();
    void malformedBadgeRecordsFailClosed();
};

void SavedLayoutStoreTests::
    versionedRoundTripPreservesExactDeviceScopedDraft() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        QDir(temporary.path()).filePath(QStringLiteral("saved-layouts"));
    Store store(directory);
    QCOMPARE(store.load().status, Store::LoadStatus::Empty);

    TryxRuntimeSavedLayoutV1 expected = layout(id(1));
    const Store::MutationResult put = store.put(0, expected);
    QVERIFY2(put.ok(), qPrintable(put.detail));
    QCOMPARE(put.revision, 1U);
    QCOMPARE(put.layout.revision, 1U);

    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        store.snapshot(QString::fromLatin1(kDeviceA),
                       QString::fromLatin1(kProduct));
    QCOMPARE(snapshot.schemaVersion, 1U);
    QCOMPARE(snapshot.revision, 1U);
    QCOMPARE(snapshot.deviceIdentity, QString::fromLatin1(kDeviceA));
    QCOMPARE(snapshot.productId, QString::fromLatin1(kProduct));
    QCOMPARE(snapshot.layouts.size(), 1);
    QCOMPARE(snapshot.layouts.constFirst(), put.layout);

    Store reloaded(directory);
    const Store::LoadResult loaded = reloaded.load();
    QCOMPARE(loaded.status, Store::LoadStatus::Loaded);
    QCOMPARE(loaded.revision, 1U);
    QCOMPARE(loaded.layouts.size(), 1);
    QCOMPARE(loaded.layouts.constFirst(), put.layout);
    QVERIFY(loaded.writesEnabled);

    struct stat status {};
    QCOMPARE(::lstat(
                 QFile::encodeName(reloaded.indexPath()).constData(),
                 &status),
             0);
    QVERIFY(S_ISREG(status.st_mode));
    QCOMPARE(status.st_nlink, static_cast<nlink_t>(1));
    QCOMPARE(status.st_mode & 0777, static_cast<mode_t>(0600));
}

void SavedLayoutStoreTests::customBadgesRoundTripAndLegacyCannotEraseThem() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Store store(directory.path());
    QCOMPARE(store.load().status, Store::LoadStatus::Empty);
    auto draft = tryxSavedLayoutV2FromV1(layout(id(16), QString::fromLatin1(kDeviceA), QStringLiteral("Custom"), true));
    draft.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Мой ПК")};
    draft.badges.secondaryGpu = {QStringLiteral("Custom"), QStringLiteral("Right GPU")};
    const auto saved = store.putV2(0, draft);
    QVERIFY2(saved.ok(), qPrintable(saved.detail));
    QCOMPARE(saved.layoutV2.badges, draft.badges);
    QVERIFY(store.layouts().isEmpty());
    QVERIFY(store.snapshot(draft.deviceIdentity, draft.productId).layouts.isEmpty());
    QCOMPARE(store.snapshotV2(draft.deviceIdentity, draft.productId).layouts.first(), saved.layoutV2);
    const QByteArray original = readFile(store.indexPath());
    auto legacy = layout(id(16), draft.deviceIdentity, draft.name, true);
    legacy.revision = saved.revision;
    QVERIFY(!store.put(saved.revision, legacy).ok());
    QVERIFY(!store.remove(saved.revision, draft.deviceIdentity, draft.productId, draft.layoutId).ok());
    QCOMPARE(readFile(store.indexPath()), original);
    Store reloaded(directory.path());
    QCOMPARE(reloaded.load().status, Store::LoadStatus::Loaded);
    QCOMPARE(reloaded.snapshotV2(draft.deviceIdentity, draft.productId).layouts.first(), saved.layoutV2);
    auto invalid = saved.layoutV2;
    invalid.badges.primaryCpu.text = QStringLiteral("private\ntext");
    const auto rejected = reloaded.putV2(saved.revision, invalid);
    QVERIFY(!rejected.ok());
    QVERIFY(!rejected.detail.contains(QStringLiteral("private")));
    QCOMPARE(readFile(store.indexPath()), original);
    QVERIFY(!reloaded.removeV2(0, draft.deviceIdentity, draft.productId, draft.layoutId).ok());
    QVERIFY(reloaded.removeV2(saved.revision, draft.deviceIdentity, draft.productId, draft.layoutId).ok());
    QVERIFY(reloaded.snapshotV2(draft.deviceIdentity, draft.productId).layouts.isEmpty());
}

void SavedLayoutStoreTests::malformedBadgeRecordsFailClosed_data() {
    QTest::addColumn<QString>("mutation");
    for (const char *name : {"missing", "future", "hidden", "invalid", "extra", "legacy-custom", "unsupported-product"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void SavedLayoutStoreTests::malformedBadgeRecordsFailClosed() {
    QFETCH(QString, mutation);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Store store(directory.path());
    auto draft = tryxSavedLayoutV2FromV1(layout(id(18)));
    draft.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("private sentinel")};
    QVERIFY(store.putV2(0, draft).ok());
    auto root = QJsonDocument::fromJson(readFile(store.indexPath())).object();
    auto records = root.value(QStringLiteral("layouts")).toArray();
    auto record = records[0].toObject();
    auto badges = record.value(QStringLiteral("badgeChoices")).toObject();
    if (mutation == QStringLiteral("future")) badges.insert(QStringLiteral("schemaVersion"), 2);
    if (mutation == QStringLiteral("hidden")) badges.insert(QStringLiteral("secondaryCpu"), badges.value(QStringLiteral("primaryCpu")));
    if (mutation == QStringLiteral("invalid")) badges.insert(QStringLiteral("primaryCpu"), QJsonObject{
        {QStringLiteral("mode"), QStringLiteral("Custom")}, {QStringLiteral("text"), QStringLiteral("private\ntext")}});
    if (mutation == QStringLiteral("extra")) badges.insert(QStringLiteral("extra"), true);
    record.insert(QStringLiteral("badgeChoices"), badges);
    if (mutation == QStringLiteral("missing")) record.remove(QStringLiteral("badgeChoices"));
    if (mutation == QStringLiteral("legacy-custom")) {
        root.insert(QStringLiteral("version"), 1);
        record.insert(QStringLiteral("schemaVersion"), 1);
    }
    if (mutation == QStringLiteral("unsupported-product")) record.insert(QStringLiteral("productId"), QStringLiteral("391a:1011"));
    records[0] = record;
    root.insert(QStringLiteral("layouts"), records);
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QVERIFY(writeFile(store.indexPath(), bytes));
    Store reloaded(directory.path());
    const auto result = reloaded.load();
    QCOMPARE(result.status, Store::LoadStatus::IgnoredMalformed);
    QVERIFY(!result.writesEnabled);
    QVERIFY(!result.detail.contains(QStringLiteral("private")));
    QVERIFY(!reloaded.putV2(0, draft).ok());
    QCOMPARE(readFile(store.indexPath()), bytes);
}

void SavedLayoutStoreTests::legacyUpgradePreservesBackup() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Store store(directory.path());
    const auto saved = store.put(0, layout(id(17)));
    QVERIFY2(saved.ok(), qPrintable(saved.detail));
    auto root = QJsonDocument::fromJson(readFile(store.indexPath())).object();
    auto records = root.value(QStringLiteral("layouts")).toArray();
    auto record = records.first().toObject();
    record.remove(QStringLiteral("badgeChoices"));
    record.insert(QStringLiteral("schemaVersion"), 1);
    records[0] = record;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("layouts"), records);
    const QByteArray original = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QVERIFY(writeFile(store.indexPath(), original));
    Store reloaded(directory.path());
    QCOMPARE(reloaded.load().status, Store::LoadStatus::Loaded);
    QCOMPARE(readFile(store.indexPath()), original);
    auto draft = reloaded.snapshotV2(QString::fromLatin1(kDeviceA), QString::fromLatin1(kProduct)).layouts.first();
    QCOMPARE(draft.badges, TryxRuntimeOverlayBadgesV1());
    draft.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("PC")};
    QVERIFY(reloaded.putV2(saved.revision, draft).ok());
    const QString backup = store.indexPath() + QStringLiteral(".pre-c16-")
        + QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
    QCOMPARE(readFile(backup), original);
    QCOMPARE(QJsonDocument::fromJson(readFile(store.indexPath())).object().value(QStringLiteral("version")).toInt(), 2);
}

void SavedLayoutStoreTests::
    otherDeviceLayoutsAreFilteredButNotDeleted() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    QCOMPARE(store.load().status, Store::LoadStatus::Empty);
    QVERIFY(store.put(0, layout(id(1))).ok());
    QVERIFY(store.put(
        1, layout(id(2), QString::fromLatin1(kDeviceB),
                  QStringLiteral("Work"))).ok());

    QCOMPARE(store.layouts().size(), 2);
    const auto deviceA = store.snapshot(
        QString::fromLatin1(kDeviceA), QString::fromLatin1(kProduct));
    const auto deviceB = store.snapshot(
        QString::fromLatin1(kDeviceB), QString::fromLatin1(kProduct));
    QCOMPARE(deviceA.layouts.size(), 1);
    QCOMPARE(deviceA.layouts.constFirst().name, QStringLiteral("Gaming"));
    QCOMPARE(deviceB.layouts.size(), 1);
    QCOMPARE(deviceB.layouts.constFirst().name, QStringLiteral("Work"));

    const auto invalidIdentity = store.snapshot(
        QStringLiteral("device\ncontrol"),
        QString::fromLatin1(kProduct));
    QCOMPARE(invalidIdentity.status, QStringLiteral("Unavailable"));
    QCOMPARE(invalidIdentity.revision, 2U);
    QVERIFY(!invalidIdentity.diagnostic.isEmpty());
    QVERIFY(invalidIdentity.layouts.isEmpty());

    QVERIFY(store.remove(
        2, QString::fromLatin1(kDeviceA),
        QString::fromLatin1(kProduct), id(1)).ok());
    QCOMPARE(store.layouts().size(), 1);
    QCOMPARE(store.snapshot(
                 QString::fromLatin1(kDeviceB),
                 QString::fromLatin1(kProduct)).layouts.size(),
             1);
}

void SavedLayoutStoreTests::
    snapshotRejectsNonCanonicalDeviceIdentity_data() {
    QTest::addColumn<QString>("deviceIdentity");

    QTest::newRow("leading-whitespace")
        << QStringLiteral(" PASE-DEVICE-A");
    QTest::newRow("trailing-whitespace")
        << QStringLiteral("PASE-DEVICE-A ");
    QTest::newRow("control")
        << QStringLiteral("PASE\nDEVICE-A");
    QTest::newRow("bidi-override")
        << QStringLiteral("PASE\u202EDEVICE-A");
    QTest::newRow("bidi-isolate")
        << QStringLiteral("PASE\u2066DEVICE-A");
    QTest::newRow("legacy-prefix")
        << QStringLiteral("legacy:PASE-DEVICE-A");
    QTest::newRow("overlong-257")
        << QString(257, QLatin1Char('a'));
}

void SavedLayoutStoreTests::
    snapshotRejectsNonCanonicalDeviceIdentity() {
    QFETCH(QString, deviceIdentity);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    QCOMPARE(store.load().status, Store::LoadStatus::Empty);
    QVERIFY(store.put(0, layout(id(1))).ok());

    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        store.snapshot(deviceIdentity, QString::fromLatin1(kProduct));
    QCOMPARE(snapshot.status, QStringLiteral("Unavailable"));
    QCOMPARE(snapshot.revision, 1U);
    QCOMPARE(snapshot.deviceIdentity, deviceIdentity);
    QVERIFY(!snapshot.diagnostic.isEmpty());
    QVERIFY(snapshot.layouts.isEmpty());
    QCOMPARE(store.layouts().size(), 1);
}

void SavedLayoutStoreTests::
    deleteRequiresExactDeviceAndProductScope() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    store.load();
    QVERIFY(store.put(0, layout(id(1))).ok());

    TryxRuntimeSavedLayoutV1 otherProduct = layout(id(2));
    otherProduct.productId = QStringLiteral("391a:1011");
    QVERIFY(store.put(1, otherProduct).ok());
    QCOMPARE(store.snapshot(
                 QString::fromLatin1(kDeviceA),
                 QStringLiteral("391a:1011")).layouts.size(),
             1);
    QCOMPARE(store.snapshot(
                 QString::fromLatin1(kDeviceA),
                 QString::fromLatin1(kProduct)).layouts.size(),
             1);

    QCOMPARE(store.remove(
                 2, QString::fromLatin1(kDeviceA),
                 QString::fromLatin1(kProduct), id(2)).code,
             Store::ErrorCode::InvalidInput);
    QCOMPARE(store.revision(), 2U);
    QCOMPARE(store.layouts().size(), 2);

    QVERIFY(store.remove(
        2, QString::fromLatin1(kDeviceA),
        QStringLiteral("391a:1011"), id(2)).ok());
    QCOMPARE(store.revision(), 3U);
    QCOMPARE(store.layouts().size(), 1);
}

void SavedLayoutStoreTests::
    putUsesSnapshotCasAndCaseInsensitiveNames() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    store.load();
    const Store::MutationResult first = store.put(0, layout(id(1)));
    QVERIFY(first.ok());

    const Store::MutationResult stale = store.put(
        0, layout(id(2), QString::fromLatin1(kDeviceA),
                  QStringLiteral("Other")));
    QCOMPARE(stale.code, Store::ErrorCode::RevisionConflict);
    QCOMPARE(store.revision(), 1U);

    const Store::MutationResult duplicate = store.put(
        1, layout(id(2), QString::fromLatin1(kDeviceA),
                  QStringLiteral("gAmInG")));
    QCOMPARE(duplicate.code, Store::ErrorCode::NameConflict);
    QCOMPARE(store.revision(), 1U);

    TryxRuntimeSavedLayoutV1 update = first.layout;
    update.request.display.brightness = 75;
    const Store::MutationResult updated = store.put(1, update);
    QVERIFY2(updated.ok(), qPrintable(updated.detail));
    QCOMPARE(updated.layout.layoutId, first.layout.layoutId);
    QCOMPARE(updated.layout.revision, 2U);
    QCOMPARE(updated.layout.request.display.brightness, 75);
}

void SavedLayoutStoreTests::
    updateAndDeleteAreAtomicAcrossRestart() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    store.load();
    const Store::MutationResult first = store.put(0, layout(id(1)));
    QVERIFY(first.ok());
    const QByteArray firstBytes = readFile(store.indexPath());
    QVERIFY(!firstBytes.isEmpty());

    TryxRuntimeSavedLayoutV1 update = first.layout;
    update.request.display.brightness = 43;
    const Store::MutationResult second = store.put(1, update);
    QVERIFY(second.ok());
    QVERIFY(readFile(store.indexPath()) != firstBytes);

    Store reloaded(temporary.path());
    QCOMPARE(reloaded.load().status, Store::LoadStatus::Loaded);
    QCOMPARE(reloaded.layouts().size(), 1);
    QCOMPARE(reloaded.layouts().constFirst().name,
             QStringLiteral("Gaming"));
    QCOMPARE(reloaded.layouts().constFirst()
                 .request.display.brightness,
             43);
    QVERIFY(reloaded.remove(
        2, QString::fromLatin1(kDeviceA),
        QString::fromLatin1(kProduct), id(1)).ok());

    Store empty(temporary.path());
    QCOMPARE(empty.load().status, Store::LoadStatus::Loaded);
    QCOMPARE(empty.revision(), 3U);
    QVERIFY(empty.layouts().isEmpty());
}

void SavedLayoutStoreTests::
    recordRevisionAndDeviceScopeAreImmutable() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    store.load();
    const Store::MutationResult first = store.put(0, layout(id(1)));
    QVERIFY(first.ok());

    TryxRuntimeSavedLayoutV1 staleRecord = first.layout;
    staleRecord.revision = 0;
    QCOMPARE(store.put(1, staleRecord).code,
             Store::ErrorCode::RevisionConflict);
    QCOMPARE(store.revision(), 1U);

    TryxRuntimeSavedLayoutV1 moved = first.layout;
    moved.deviceIdentity = QString::fromLatin1(kDeviceB);
    QCOMPARE(store.put(1, moved).code, Store::ErrorCode::InvalidInput);
    QCOMPARE(store.revision(), 1U);

    TryxRuntimeSavedLayoutV1 renamed = first.layout;
    renamed.name = QStringLiteral("Renamed");
    QCOMPARE(store.put(1, renamed).code,
             Store::ErrorCode::InvalidInput);
    QCOMPARE(store.revision(), 1U);

    TryxRuntimeSavedLayoutV1 duplicateId = layout(
        first.layout.layoutId, QString::fromLatin1(kDeviceB),
        QStringLiteral("Other device"));
    QCOMPARE(store.put(1, duplicateId).code,
             Store::ErrorCode::RevisionConflict);
    QCOMPARE(store.layouts(), QList<TryxRuntimeSavedLayoutV1>{first.layout});
}

void SavedLayoutStoreTests::
    limitsAndCanonicalSerializationAreEnforced() {
    QTemporaryDir firstTemporary;
    QTemporaryDir secondTemporary;
    QVERIFY(firstTemporary.isValid());
    QVERIFY(secondTemporary.isValid());
    Store first(firstTemporary.path());
    Store second(secondTemporary.path());
    first.load();
    second.load();
    const TryxRuntimeSavedLayoutV1 same = layout(id(1));
    QVERIFY(first.put(0, same).ok());
    QVERIFY(second.put(0, same).ok());
    QCOMPARE(readFile(first.indexPath()), readFile(second.indexPath()));

    const QJsonObject root = QJsonDocument::fromJson(
        readFile(first.indexPath())).object();
    QCOMPARE(root.keys(),
             QStringList({QStringLiteral("layouts"),
                          QStringLiteral("revision"),
                          QStringLiteral("version")}));
    QVERIFY(root.value(QStringLiteral("revision")).isString());
    QCOMPARE(root.value(QStringLiteral("revision")).toString(),
             QStringLiteral("1"));

    for (int index = 2; index <= 32; ++index) {
        TryxRuntimeSavedLayoutV1 next = layout(
            id(index), QString::fromLatin1(kDeviceA),
            QStringLiteral("Layout %1").arg(index));
        QVERIFY2(first.put(first.revision(), next).ok(),
                 qPrintable(QStringLiteral("index=%1").arg(index)));
    }
    TryxRuntimeSavedLayoutV1 overDeviceLimit = layout(
        id(33), QString::fromLatin1(kDeviceA),
        QStringLiteral("Layout 33"));
    QCOMPARE(first.put(first.revision(), overDeviceLimit).code,
             Store::ErrorCode::ResourceLimitExceeded);
    QCOMPARE(first.layouts().size(), 32);

    for (int index = 33; index <= 256; ++index) {
        TryxRuntimeSavedLayoutV1 next = layout(
            id(index),
            QStringLiteral("PASE-DEVICE-%1").arg(index / 31),
            QStringLiteral("Layout %1").arg(index));
        QVERIFY2(first.put(first.revision(), next).ok(),
                 qPrintable(QStringLiteral("total-index=%1").arg(index)));
    }
    QCOMPARE(first.layouts().size(), 256);
    TryxRuntimeSavedLayoutV1 overTotalLimit = layout(
        id(257), QStringLiteral("PASE-DEVICE-99"),
        QStringLiteral("Layout 257"));
    QCOMPARE(first.put(first.revision(), overTotalLimit).code,
             Store::ErrorCode::ResourceLimitExceeded);
    QCOMPARE(first.layouts().size(), 256);
}

void SavedLayoutStoreTests::invalidLayoutNeverMutatesStore_data() {
    QTest::addColumn<QString>("caseName");
    QTest::newRow("bad-id") << QStringLiteral("bad-id");
    QTest::newRow("legacy-device") << QStringLiteral("legacy-device");
    QTest::newRow("control-device") << QStringLiteral("control-device");
    QTest::newRow("bidi-device") << QStringLiteral("bidi-device");
    QTest::newRow("turris") << QStringLiteral("turris");
    QTest::newRow("missing-media-id") << QStringLiteral("missing-media-id");
    QTest::newRow("media-name-drift") << QStringLiteral("media-name-drift");
    QTest::newRow("backlight") << QStringLiteral("backlight");
    QTest::newRow("standby") << QStringLiteral("standby");
    QTest::newRow("bad-play-mode") << QStringLiteral("bad-play-mode");
    QTest::newRow("too-long-name") << QStringLiteral("too-long-name");
    QTest::newRow("whitespace-name")
        << QStringLiteral("whitespace-name");
    QTest::newRow("control-name") << QStringLiteral("control-name");
    QTest::newRow("bidi-name") << QStringLiteral("bidi-name");
    QTest::newRow("uppercase-media-id")
        << QStringLiteral("uppercase-media-id");
    QTest::newRow("mismatched-media-id")
        << QStringLiteral("mismatched-media-id");
    QTest::newRow("bad-source") << QStringLiteral("bad-source");
    QTest::newRow("full-two-media") << QStringLiteral("full-two-media");
    QTest::newRow("split-one-media") << QStringLiteral("split-one-media");
    QTest::newRow("missing-brightness")
        << QStringLiteral("missing-brightness");
    QTest::newRow("missing-orientation")
        << QStringLiteral("missing-orientation");
    QTest::newRow("waterfall-drift")
        << QStringLiteral("waterfall-drift");
    QTest::newRow("preset") << QStringLiteral("preset");
    QTest::newRow("filter") << QStringLiteral("filter");
    QTest::newRow("uppercase-color")
        << QStringLiteral("uppercase-color");
}

void SavedLayoutStoreTests::invalidLayoutNeverMutatesStore() {
    QFETCH(QString, caseName);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Store store(temporary.path());
    store.load();
    TryxRuntimeSavedLayoutV1 invalid = layout(id(1));
    if (caseName == QStringLiteral("bad-id")) {
        invalid.layoutId = QStringLiteral("not-a-uuid");
    } else if (caseName == QStringLiteral("legacy-device")) {
        invalid.deviceIdentity = QStringLiteral("legacy:serial");
    } else if (caseName == QStringLiteral("control-device")) {
        invalid.deviceIdentity = QStringLiteral("bad\nserial");
    } else if (caseName == QStringLiteral("bidi-device")) {
        invalid.deviceIdentity = QStringLiteral("bad\u202Eserial");
    } else if (caseName == QStringLiteral("turris")) {
        invalid.productId = QStringLiteral("391a:2011");
    } else if (caseName == QStringLiteral("missing-media-id")) {
        invalid.media[0].mediaId.clear();
    } else if (caseName == QStringLiteral("media-name-drift")) {
        invalid.request.media[0] = QStringLiteral("other.h264");
    } else if (caseName == QStringLiteral("backlight")) {
        invalid.request.display.backlightPresent = true;
    } else if (caseName == QStringLiteral("standby")) {
        invalid.request.display.standbyPresent = true;
    } else if (caseName == QStringLiteral("bad-play-mode")) {
        invalid.request.playMode = QStringLiteral("Random");
    } else if (caseName == QStringLiteral("too-long-name")) {
        invalid.name = QString(81, QLatin1Char('x'));
    } else if (caseName == QStringLiteral("whitespace-name")) {
        invalid.name = QStringLiteral(" Gaming ");
    } else if (caseName == QStringLiteral("control-name")) {
        invalid.name = QStringLiteral("Bad\nname");
    } else if (caseName == QStringLiteral("bidi-name")) {
        invalid.name = QStringLiteral("Bad\u202Ename");
    } else if (caseName == QStringLiteral("uppercase-media-id")) {
        invalid.media[0].mediaId = QString(64, QLatin1Char('A'));
    } else if (caseName == QStringLiteral("mismatched-media-id")) {
        invalid.media[0].mediaId = QString(64, QLatin1Char('a'));
    } else if (caseName == QStringLiteral("bad-source")) {
        invalid.media[0].source = 3;
    } else if (caseName == QStringLiteral("full-two-media")) {
        invalid.media.append(
            mediaRef(invalid.deviceIdentity,
                     QStringLiteral("second.h264_2240x1080"),
                     2048));
        invalid.request.media.append(invalid.media.constLast().name);
    } else if (caseName == QStringLiteral("split-one-media")) {
        invalid.request.screenMode = QStringLiteral("Screen Splitting");
        invalid.request.playMode = QStringLiteral("Single");
    } else if (caseName == QStringLiteral("missing-brightness")) {
        invalid.request.display.brightnessPresent = false;
    } else if (caseName == QStringLiteral("missing-orientation")) {
        invalid.request.display.orientationPresent = false;
    } else if (caseName == QStringLiteral("waterfall-drift")) {
        invalid.request.waterfallMode =
            !invalid.request.display.waterfallMode;
    } else if (caseName == QStringLiteral("preset")) {
        invalid.request.presetId = QStringLiteral("vendor-preset");
    } else if (caseName == QStringLiteral("filter")) {
        invalid.request.filterOpacity = 1;
    } else if (caseName == QStringLiteral("uppercase-color")) {
        invalid.request.settingsColor = QStringLiteral("#AABBCC");
    }

    const Store::MutationResult result = store.put(0, invalid);
    QCOMPARE(result.code, Store::ErrorCode::InvalidInput);
    QCOMPARE(store.revision(), 0U);
    QVERIFY(store.layouts().isEmpty());
    QVERIFY(!QFileInfo::exists(store.indexPath()));
}

void SavedLayoutStoreTests::
    malformedFutureOversizedAndUnsafeStateFailClosed_data() {
    QTest::addColumn<QString>("caseName");
    QTest::newRow("malformed") << QStringLiteral("malformed");
    QTest::newRow("future") << QStringLiteral("future");
    QTest::newRow("oversized") << QStringLiteral("oversized");
    QTest::newRow("unknown-keys") << QStringLiteral("unknown-keys");
    QTest::newRow("symlink") << QStringLiteral("symlink");
    QTest::newRow("hardlink") << QStringLiteral("hardlink");
}

void SavedLayoutStoreTests::
    malformedFutureOversizedAndUnsafeStateFailClosed() {
    QFETCH(QString, caseName);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        QDir(temporary.path()).filePath(QStringLiteral("saved-layouts"));
    QVERIFY(QDir().mkpath(directory));
    const QString index =
        QDir(directory).filePath(QStringLiteral("index.json"));
    if (caseName == QStringLiteral("malformed")) {
        QVERIFY(writeFile(index, QByteArrayLiteral("{")));
    } else if (caseName == QStringLiteral("future")) {
        QVERIFY(writeFile(index, QByteArrayLiteral(
            "{\"layouts\":[],\"revision\":\"1\",\"version\":3}")));
    } else if (caseName == QStringLiteral("oversized")) {
        QVERIFY(writeFile(index, QByteArray(1024 * 1024 + 1, 'x')));
    } else if (caseName == QStringLiteral("unknown-keys")) {
        QVERIFY(writeFile(index, QByteArrayLiteral(
            "{\"extra\":true,\"layouts\":[],\"revision\":\"1\",\"version\":1}")));
    } else if (caseName == QStringLiteral("symlink")) {
        const QString target =
            QDir(temporary.path()).filePath(QStringLiteral("target"));
        QVERIFY(writeFile(target, QByteArrayLiteral("{}")));
        QVERIFY(QFile::link(target, index));
    } else if (caseName == QStringLiteral("hardlink")) {
        const QString target =
            QDir(temporary.path()).filePath(QStringLiteral("target"));
        QVERIFY(writeFile(target, QByteArrayLiteral("{}")));
        QCOMPARE(::link(QFile::encodeName(target).constData(),
                        QFile::encodeName(index).constData()),
                 0);
    }

    Store store(directory);
    const Store::LoadResult loaded = store.load();
    QVERIFY(loaded.status != Store::LoadStatus::Empty);
    QVERIFY(loaded.status != Store::LoadStatus::Loaded);
    QVERIFY(!loaded.writesEnabled);
    QVERIFY(!store.writesEnabled());
    QVERIFY(store.layouts().isEmpty());
    QCOMPARE(store.put(0, layout(id(1))).code,
             Store::ErrorCode::WritesDisabled);
}

void SavedLayoutStoreTests::
    postCommitDirectoryRaceReportsUnknownAndDisablesWrites() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory =
        QDir(temporary.path()).filePath(QStringLiteral("saved-layouts"));
    Store store(directory);
    store.load();
    QVERIFY(store.put(0, layout(id(1))).ok());

    const QString moved = directory + QStringLiteral("-moved");
    store.setAfterCommitHookForTesting([directory, moved]() {
        QVERIFY(QDir().rename(directory, moved));
        QVERIFY(QDir().mkpath(directory));
    });
    const Store::MutationResult raced = store.put(
        1, layout(id(2), QString::fromLatin1(kDeviceA),
                  QStringLiteral("Second")));
    QCOMPARE(raced.code, Store::ErrorCode::CommitUnknown);
    QVERIFY(raced.commitMayExist);
    QVERIFY(!store.writesEnabled());
    QCOMPARE(store.revision(), 1U);
    QCOMPARE(store.layouts().size(), 1);
    QCOMPARE(store.put(1, layout(id(3))).code,
             Store::ErrorCode::WritesDisabled);
}

QTEST_GUILESS_MAIN(SavedLayoutStoreTests)

#include "savedlayoutstore_tests.moc"
