#include "replacejournal.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

namespace {

TryxReplaceJournalRecord validRecord() {
    TryxReplaceJournalRecord record;
    record.productId = 0x1021;
    record.operationId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    record.deviceIdentity = QStringLiteral("PASE-TEST-001");
    record.deviceGeneration = 7;
    record.originalMediaId = QString(64, QLatin1Char('a'));
    record.originalRemoteName =
        QStringLiteral("original.mp4.h264_2240x1080");
    record.originalSize = 123456;
    record.artifactId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    record.decodedSha256 = QString(64, QLatin1Char('b'));
    record.transformFingerprint = QString(64, QLatin1Char('c'));
    record.applyFingerprint = QString(64, QLatin1Char('d'));
    record.referenceNames = {
        QStringLiteral("original.mp4.h264_2240x1080")};
    return record;
}

bool writeRawFile(const QString &path, const QByteArray &payload,
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

}  // namespace

class ReplaceJournalTests final : public QObject {
    Q_OBJECT

private slots:
    void missingJournalIsNotAnError();
    void ownerOnlyRoundTripAndClear();
    void version2BindsSupportedProduct_data();
    void version2BindsSupportedProduct();
    void legacyVersion1LoadsAs1021AndUpgradesOnTransition();
    void version2ProductIdentityIsExactAndImmutable();
    void duplicateTopLevelKeysFailClosed_data();
    void duplicateTopLevelKeysFailClosed();
    void malformedAndUnexpectedFieldsFailClosed();
    void unsafeFilesystemEntriesFailClosed();
    void invalidWriteDoesNotReplaceValidJournal();
    void transitionsAreMonotonic();
    void terminalOriginalRetainedIsValidAndImmutable();
    void oversizedJournalIsRejected();
};

void ReplaceJournalTests::missingJournalIsNotAnError() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TryxReplaceJournal journal(
        directory.filePath(QStringLiteral("replace-intent.json")));

    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Missing);
    QVERIFY(loaded.error.isEmpty());
    QString error;
    QVERIFY2(journal.clear(&error), qPrintable(error));
}

void ReplaceJournalTests::ownerOnlyRoundTripAndClear() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    const TryxReplaceJournalRecord expected = validRecord();

    QString error;
    QVERIFY2(journal.write(expected, &error), qPrintable(error));

    struct stat status {};
    QVERIFY(::lstat(QFile::encodeName(path).constData(), &status) == 0);
    QVERIFY(S_ISREG(status.st_mode));
    QCOMPARE(status.st_uid, ::geteuid());
    QCOMPARE(status.st_mode & 07777,
             static_cast<mode_t>(S_IRUSR | S_IWUSR));
    QCOMPARE(status.st_nlink, static_cast<nlink_t>(1));
    QVERIFY(status.st_size > 0);
    QVERIFY(status.st_size <= TryxReplaceJournal::MaximumBytes);

    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Loaded);
    QVERIFY(loaded.error.isEmpty());
    QCOMPARE(loaded.record.formatVersion,
             TryxReplaceJournal::FormatVersion);
    QCOMPARE(loaded.record.productId, expected.productId);
    QCOMPARE(loaded.record.operationId, expected.operationId);
    QCOMPARE(loaded.record.deviceIdentity, expected.deviceIdentity);
    QCOMPARE(loaded.record.deviceGeneration,
             expected.deviceGeneration);
    QCOMPARE(loaded.record.originalMediaId, expected.originalMediaId);
    QCOMPARE(loaded.record.originalRemoteName,
             expected.originalRemoteName);
    QCOMPARE(loaded.record.originalSize, expected.originalSize);
    QCOMPARE(loaded.record.artifactId, expected.artifactId);
    QCOMPARE(loaded.record.decodedSha256, expected.decodedSha256);
    QCOMPARE(loaded.record.transformFingerprint,
             expected.transformFingerprint);
    QCOMPARE(loaded.record.applyFingerprint,
             expected.applyFingerprint);
    QCOMPARE(loaded.record.referenceNames, expected.referenceNames);
    QCOMPARE(loaded.record.stage, QStringLiteral("Preflight"));
    QCOMPARE(loaded.record.disposition,
             QStringLiteral("OriginalRetained"));

    QVERIFY2(journal.clear(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(journal.load().status,
             TryxReplaceJournalLoadStatus::Missing);
}

void ReplaceJournalTests::version2BindsSupportedProduct_data() {
    QTest::addColumn<quint16>("productId");
    QTest::addColumn<QString>("encodedProductId");

    QTest::newRow("panorama-1011")
        << quint16{0x1011} << QStringLiteral("1011");
    QTest::newRow("pase-1021")
        << quint16{0x1021} << QStringLiteral("1021");
}

void ReplaceJournalTests::version2BindsSupportedProduct() {
    QFETCH(quint16, productId);
    QFETCH(QString, encodedProductId);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    TryxReplaceJournalRecord expected = validRecord();
    expected.productId = productId;

    QString error;
    QVERIFY2(journal.write(expected, &error), qPrintable(error));
    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Loaded);
    QCOMPARE(loaded.record.formatVersion,
             TryxReplaceJournal::FormatVersion);
    QCOMPARE(loaded.record.productId, productId);

    const QJsonDocument document =
        QJsonDocument::fromJson(readFile(path));
    QVERIFY(document.isObject());
    QCOMPARE(document.object().value(
                 QStringLiteral("version")).toInt(),
             TryxReplaceJournal::FormatVersion);
    QCOMPARE(document.object().value(
                 QStringLiteral("productId")).toString(),
             encodedProductId);
}

void ReplaceJournalTests::
    legacyVersion1LoadsAs1021AndUpgradesOnTransition() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    QString error;
    QVERIFY2(journal.write(validRecord(), &error), qPrintable(error));

    QJsonDocument document =
        QJsonDocument::fromJson(readFile(path));
    QVERIFY(document.isObject());
    QJsonObject legacy = document.object();
    legacy.insert(QStringLiteral("version"),
                  TryxReplaceJournal::LegacyFormatVersion);
    legacy.remove(QStringLiteral("productId"));
    const QByteArray legacyBytes =
        QJsonDocument(legacy).toJson(QJsonDocument::Compact);
    QVERIFY(writeRawFile(
        path, legacyBytes,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Loaded);
    QCOMPARE(loaded.record.formatVersion,
             TryxReplaceJournal::LegacyFormatVersion);
    QCOMPARE(loaded.record.productId, quint16{0x1021});
    QCOMPARE(readFile(path), legacyBytes);

    QVERIFY(!journal.write(loaded.record, &error));
    QCOMPARE(readFile(path), legacyBytes);

    TryxReplaceJournalRecord formatOnlyUpgrade = loaded.record;
    formatOnlyUpgrade.formatVersion =
        TryxReplaceJournal::FormatVersion;
    QVERIFY(!journal.write(formatOnlyUpgrade, &error));
    QCOMPARE(readFile(path), legacyBytes);

    TryxReplaceJournalRecord upgraded = loaded.record;
    upgraded.formatVersion = TryxReplaceJournal::FormatVersion;
    upgraded.stage = QStringLiteral("Preparing");
    QVERIFY2(journal.write(upgraded, &error), qPrintable(error));
    const TryxReplaceJournalLoadResult reloaded = journal.load();
    QCOMPARE(reloaded.status, TryxReplaceJournalLoadStatus::Loaded);
    QCOMPARE(reloaded.record.formatVersion,
             TryxReplaceJournal::FormatVersion);
    QCOMPARE(reloaded.record.productId, quint16{0x1021});
    document = QJsonDocument::fromJson(readFile(path));
    QCOMPARE(document.object().value(
                 QStringLiteral("productId")).toString(),
             QStringLiteral("1021"));

    const QString terminalPath =
        directory.filePath(QStringLiteral("terminal-replace-intent.json"));
    QJsonObject terminalLegacy = legacy;
    terminalLegacy.insert(QStringLiteral("stage"),
                          QStringLiteral("Terminal"));
    terminalLegacy.insert(QStringLiteral("disposition"),
                          QStringLiteral("OriginalRetained"));
    const QByteArray terminalLegacyBytes =
        QJsonDocument(terminalLegacy).toJson(QJsonDocument::Compact);
    QVERIFY(writeRawFile(
        terminalPath, terminalLegacyBytes,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    TryxReplaceJournal terminalJournal(terminalPath);
    const TryxReplaceJournalLoadResult loadedTerminal =
        terminalJournal.load();
    QCOMPARE(loadedTerminal.status,
             TryxReplaceJournalLoadStatus::Loaded);
    TryxReplaceJournalRecord terminalFormatOnly =
        loadedTerminal.record;
    terminalFormatOnly.formatVersion =
        TryxReplaceJournal::FormatVersion;
    QVERIFY(!terminalJournal.write(terminalFormatOnly, &error));
    QCOMPARE(readFile(terminalPath), terminalLegacyBytes);
}

void ReplaceJournalTests::
    version2ProductIdentityIsExactAndImmutable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    TryxReplaceJournalRecord initial = validRecord();
    initial.productId = 0x1011;
    QString error;
    QVERIFY2(journal.write(initial, &error), qPrintable(error));
    const QByteArray committed = readFile(path);

    TryxReplaceJournalRecord changed = initial;
    changed.productId = 0x1021;
    changed.stage = QStringLiteral("Preparing");
    QVERIFY(!journal.write(changed, &error));
    QCOMPARE(readFile(path), committed);

    TryxReplaceJournalRecord invalid = initial;
    invalid.productId = 0;
    QVERIFY(!TryxReplaceJournal::validateRecord(invalid, &error));
    invalid.productId = 0x2011;
    QVERIFY(!TryxReplaceJournal::validateRecord(invalid, &error));

    const auto ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QJsonDocument document = QJsonDocument::fromJson(committed);
    QVERIFY(document.isObject());
    const QJsonObject canonical = document.object();
    const auto expectInvalid =
        [&journal, &path, ownerOnly](QJsonObject object) {
            const QByteArray payload =
                QJsonDocument(object).toJson(QJsonDocument::Compact);
            QVERIFY(writeRawFile(
                path, payload, ownerOnly));
            const TryxReplaceJournalLoadResult loaded = journal.load();
            QCOMPARE(loaded.status,
                     TryxReplaceJournalLoadStatus::Invalid);
            QCOMPARE(readFile(path), payload);
            QString mutationError;
            QVERIFY(!journal.write(validRecord(), &mutationError));
            QVERIFY(!mutationError.isEmpty());
            QCOMPARE(readFile(path), payload);
            QVERIFY(!journal.clear(&mutationError));
            QVERIFY(!mutationError.isEmpty());
            QCOMPARE(readFile(path), payload);
        };

    QJsonObject malformed = canonical;
    malformed.remove(QStringLiteral("productId"));
    expectInvalid(malformed);
    malformed = canonical;
    malformed.insert(QStringLiteral("productId"), 0x1011);
    expectInvalid(malformed);
    malformed = canonical;
    malformed.insert(QStringLiteral("productId"),
                     QStringLiteral("0x1011"));
    expectInvalid(malformed);
    malformed = canonical;
    malformed.insert(QStringLiteral("productId"),
                     QStringLiteral("2011"));
    expectInvalid(malformed);
    malformed = canonical;
    malformed.insert(QStringLiteral("version"),
                     TryxReplaceJournal::LegacyFormatVersion);
    expectInvalid(malformed);
    malformed = canonical;
    malformed.insert(QStringLiteral("version"),
                     TryxReplaceJournal::FormatVersion + 1);
    expectInvalid(malformed);
}

void ReplaceJournalTests::duplicateTopLevelKeysFailClosed_data() {
    QTest::addColumn<QByteArray>("duplicateSuffix");

    QTest::newRow("duplicate-version")
        << QByteArray(",\"version\":2}");
    QTest::newRow("duplicate-product")
        << QByteArray(",\"productId\":\"1021\"}");
    QTest::newRow("escaped-duplicate-product")
        << QByteArray(",\"\\u0070roductId\":\"1021\"}");
}

void ReplaceJournalTests::duplicateTopLevelKeysFailClosed() {
    QFETCH(QByteArray, duplicateSuffix);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    QString error;
    QVERIFY2(journal.write(validRecord(), &error), qPrintable(error));

    QByteArray payload = readFile(path);
    QVERIFY(payload.endsWith('}'));
    payload.chop(1);
    payload += duplicateSuffix;
    QVERIFY(writeRawFile(
        path, payload,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);
    QVERIFY(!loaded.error.isEmpty());
    QCOMPARE(readFile(path), payload);
    QVERIFY(!journal.write(validRecord(), &error));
    QCOMPARE(readFile(path), payload);
    QVERIFY(!journal.clear(&error));
    QCOMPARE(readFile(path), payload);
}

void ReplaceJournalTests::
    malformedAndUnexpectedFieldsFailClosed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    const auto ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;

    QVERIFY(writeRawFile(path, QByteArray("{"),
                         ownerOnly));
    TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);
    QVERIFY(!loaded.error.isEmpty());
    QString error;
    QVERIFY(!journal.clear(&error));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!journal.write(validRecord(), &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(QFile::remove(path));
    QVERIFY2(journal.write(validRecord(), &error), qPrintable(error));
    QJsonDocument document =
        QJsonDocument::fromJson(readFile(path));
    QVERIFY(document.isObject());
    QJsonObject object = document.object();
    object.insert(QStringLiteral("unexpected"), true);
    QVERIFY(writeRawFile(
        path, QJsonDocument(object).toJson(QJsonDocument::Compact),
        ownerOnly));
    loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);

    object.remove(QStringLiteral("unexpected"));
    object.insert(QStringLiteral("version"), 1.5);
    QVERIFY(writeRawFile(
        path, QJsonDocument(object).toJson(QJsonDocument::Compact),
        ownerOnly));
    loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);

    object.insert(QStringLiteral("version"),
                  TryxReplaceJournal::FormatVersion);
    object.insert(QStringLiteral("uploadVerified"),
                  QStringLiteral("true"));
    QVERIFY(writeRawFile(
        path, QJsonDocument(object).toJson(QJsonDocument::Compact),
        ownerOnly));
    loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);
}

void ReplaceJournalTests::unsafeFilesystemEntriesFailClosed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    const QString target =
        directory.filePath(QStringLiteral("target.json"));
    const auto ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QVERIFY(writeRawFile(target, QByteArray("{}"), ownerOnly));
    QVERIFY(::symlink(
                QFile::encodeName(target).constData(),
                QFile::encodeName(path).constData()) == 0);

    TryxReplaceJournal symlinkJournal(path);
    QCOMPARE(symlinkJournal.load().status,
             TryxReplaceJournalLoadStatus::Invalid);
    QString error;
    QVERIFY(!symlinkJournal.clear(&error));
    QVERIFY(QFileInfo(path).isSymLink());
    QVERIFY(!symlinkJournal.write(validRecord(), &error));

    QVERIFY(QFile::remove(path));
    TryxReplaceJournal modeJournal(path);
    QVERIFY2(modeJournal.write(validRecord(), &error),
             qPrintable(error));
    QVERIFY(QFile::setPermissions(
        path, QFileDevice::ReadOwner |
                  QFileDevice::WriteOwner |
                  QFileDevice::ReadGroup));
    QCOMPARE(modeJournal.load().status,
             TryxReplaceJournalLoadStatus::Invalid);
    QVERIFY(!modeJournal.clear(&error));

    QVERIFY(QFile::remove(path));
    QVERIFY2(modeJournal.write(validRecord(), &error),
             qPrintable(error));
    const QString alias =
        directory.filePath(QStringLiteral("replace-hardlink.json"));
    QVERIFY(::link(QFile::encodeName(path).constData(),
                   QFile::encodeName(alias).constData()) == 0);
    QCOMPARE(modeJournal.load().status,
             TryxReplaceJournalLoadStatus::Invalid);
    QVERIFY(!modeJournal.clear(&error));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(QFileInfo::exists(alias));
}

void ReplaceJournalTests::
    invalidWriteDoesNotReplaceValidJournal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    QString error;
    QVERIFY2(journal.write(validRecord(), &error), qPrintable(error));
    const QByteArray before = readFile(path);
    QVERIFY(!before.isEmpty());

    TryxReplaceJournalRecord invalid = validRecord();
    invalid.decodedSha256 = QStringLiteral("not-a-sha256");
    QVERIFY(!journal.write(invalid, &error));
    QCOMPARE(readFile(path), before);

    invalid = validRecord();
    invalid.newRemoteName =
        QStringLiteral("new.mp4.h264_2240x1080");
    QVERIFY(!journal.write(invalid, &error));
    QCOMPARE(readFile(path), before);
    QCOMPARE(journal.load().status,
             TryxReplaceJournalLoadStatus::Loaded);
}

void ReplaceJournalTests::transitionsAreMonotonic() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    TryxReplaceJournal journal(path);
    QString error;
    const TryxReplaceJournalRecord initial = validRecord();
    QVERIFY2(journal.write(initial, &error), qPrintable(error));

    TryxReplaceJournalRecord uploaded = initial;
    uploaded.newRemoteName =
        QStringLiteral("new.mp4.h264_2240x1080");
    uploaded.newSize = 654321;
    uploaded.uploadVerified = true;
    uploaded.stage = QStringLiteral("UploadVerified");
    uploaded.disposition = QStringLiteral("NewCopyReady");
    QVERIFY2(journal.write(uploaded, &error), qPrintable(error));

    TryxReplaceJournalRecord regressed = uploaded;
    regressed.stage = QStringLiteral("Uploading");
    QVERIFY(!journal.write(regressed, &error));

    TryxReplaceJournalRecord changedIdentity = uploaded;
    changedIdentity.artifactId =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    QVERIFY(!journal.write(changedIdentity, &error));

    TryxReplaceJournalRecord unverifiedDelete = uploaded;
    unverifiedDelete.stage =
        QStringLiteral("DeleteIntentLinked");
    unverifiedDelete.deleteIntentLinked = true;
    QVERIFY(!journal.write(unverifiedDelete, &error));

    TryxReplaceJournalRecord deleting = uploaded;
    deleting.stage = QStringLiteral("Deleting");
    deleting.applyMayHaveStarted = true;
    deleting.applyVerified = true;
    deleting.deleteIntentLinked = true;
    deleting.fileRemoveMayHaveStarted = true;
    QVERIFY2(journal.write(deleting, &error), qPrintable(error));

    TryxReplaceJournalRecord replaced = deleting;
    replaced.stage = QStringLiteral("Terminal");
    replaced.disposition = QStringLiteral("Replaced");
    QVERIFY2(journal.write(replaced, &error), qPrintable(error));

    TryxReplaceJournalRecord afterTerminal = replaced;
    afterTerminal.disposition = QStringLiteral("PartialOrUnknown");
    QVERIFY(!journal.write(afterTerminal, &error));
}

void ReplaceJournalTests::
    terminalOriginalRetainedIsValidAndImmutable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TryxReplaceJournal journal(
        directory.filePath(QStringLiteral("replace-intent.json")));
    QString error;
    const TryxReplaceJournalRecord initial = validRecord();
    QVERIFY2(journal.write(initial, &error), qPrintable(error));

    TryxReplaceJournalRecord uncertainTerminal = initial;
    uncertainTerminal.stage = QStringLiteral("Terminal");
    uncertainTerminal.disposition =
        QStringLiteral("PartialOrUnknown");
    QVERIFY(!journal.write(uncertainTerminal, &error));

    TryxReplaceJournalRecord terminal = initial;
    terminal.stage = QStringLiteral("Terminal");
    QVERIFY2(journal.write(terminal, &error), qPrintable(error));
    QCOMPARE(journal.load().record.disposition,
             QStringLiteral("OriginalRetained"));

    TryxReplaceJournalRecord changed = terminal;
    changed.disposition = QStringLiteral("PartialOrUnknown");
    QVERIFY(!journal.write(changed, &error));
    QVERIFY2(journal.write(terminal, &error), qPrintable(error));
}

void ReplaceJournalTests::oversizedJournalIsRejected() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        directory.filePath(QStringLiteral("replace-intent.json"));
    const QByteArray oversized(
        TryxReplaceJournal::MaximumBytes + 1, 'x');
    QVERIFY(writeRawFile(
        path, oversized,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    TryxReplaceJournal journal(path);
    const TryxReplaceJournalLoadResult loaded = journal.load();
    QCOMPARE(loaded.status, TryxReplaceJournalLoadStatus::Invalid);
    QVERIFY(!loaded.error.isEmpty());
}

QTEST_APPLESS_MAIN(ReplaceJournalTests)

#include "replacejournal_tests.moc"
