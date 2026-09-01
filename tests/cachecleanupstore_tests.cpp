#include "devicemediaartifactstore.h"
#include "mediacatalogstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using ArtifactStore = tryx::DeviceMediaArtifactStore;
using CatalogStore = tryx::MediaCatalogStore;

bool writeFile(const QString &path, const QByteArray &payload) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(payload) != payload.size()) {
        return false;
    }
    file.close();
    return QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QString sha256(const QByteArray &payload) {
    return QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
            .toHex());
}

CatalogStore::RemoteEntry remoteEntry() {
    CatalogStore::RemoteEntry remote;
    remote.name = QStringLiteral("accepted.png.h264_2240x1080");
    remote.size = 4096;
    remote.source = 1;
    remote.readOnly = false;
    return remote;
}

QString canonicalId(int suffix) {
    return QStringLiteral("11111111-1111-4111-8111-%1")
        .arg(suffix, 12, 10, QLatin1Char('0'));
}

ArtifactStore::ReservationInput reservation(const QString &artifactId,
                                            quint64 size) {
    ArtifactStore::ReservationInput input;
    input.artifactId = artifactId;
    input.operationId = canonicalId(90);
    input.mediaId = QString(64, QLatin1Char('a'));
    input.deviceIdentity = QStringLiteral("PASE-DEVICE-A");
    input.remoteName = QStringLiteral("clip.h264_2240x1080");
    input.expectedSize = size;
    input.logicalType = QStringLiteral("video/h264");
    input.ownerUniqueName = QStringLiteral(":1.90");
    return input;
}

ArtifactStore::ArtifactResult makeReadyArtifact(
    ArtifactStore &store, const QString &artifactId,
    const QByteArray &payload, bool releaseOperationHold = true) {
    const auto reserved = store.reserve(
        reservation(artifactId, static_cast<quint64>(payload.size())));
    if (!reserved.ok() ||
        !writeFile(reserved.artifact.canonicalPath, payload)) {
        return {};
    }
    ArtifactStore::FinalizeInput finalized;
    finalized.artifactId = artifactId;
    finalized.operationId = canonicalId(90);
    finalized.remoteName = QStringLiteral("clip.h264_2240x1080");
    finalized.outputPath = reserved.artifact.canonicalPath;
    finalized.fileSize = payload.size();
    finalized.chunkCount = 1;
    finalized.rawSha256 = sha256(payload);
    finalized.decodedSha256 = sha256(payload);
    if (!store.finalize(finalized).ok() ||
        (releaseOperationHold &&
         !store.releaseOperationHold(artifactId, canonicalId(90)).ok())) {
        return {};
    }
    return store.artifact(artifactId);
}

}  // namespace

namespace tryx::printer_media_identity {

bool isSafePrinterUploadMediaName(const QString &fileName) {
    return !fileName.isEmpty() && !fileName.contains(QLatin1Char('/')) &&
        !fileName.contains(QLatin1Char('\\'));
}

bool isCanonicalPrinterConversionProfile(
    const QString &conversionProfile) {
    return !conversionProfile.isEmpty();
}

bool printerConversionProfileMatchesMediaName(
    const QString &conversionProfile, const QString &fileName) {
    return !conversionProfile.isEmpty() && !fileName.isEmpty();
}

}  // namespace tryx::printer_media_identity

class CacheCleanupStoreTests final : public QObject {
    Q_OBJECT

private slots:
    void catalogCleanupEmptySuccessIsExact();
    void catalogCleanupPreservesAcceptedIndexAndReportsExactBytes();
    void catalogCleanupFailsClosedForUnsafeCandidateAndBound();
    void catalogCleanupRejectsHardlinkAndSpecialCandidates_data();
    void catalogCleanupRejectsHardlinkAndSpecialCandidates();
    void catalogCleanupRequiresAcceptedLoadAndPinsParentIdentity();
    void catalogCleanupRejectsChangedLeafIdentity();
    void catalogCleanupReportsConfirmedMutationBeforeFsyncFailure();
    void artifactCleanupEmptySuccessIsExact();
    void artifactCleanupRequiresExpiryAndReportsExactBytes();
    void artifactCleanupBlocksActiveLeaseAndPreservesHeldFile();
    void artifactCleanupBlocksReservationAndOperationHold();
    void artifactCleanupDefersOwnerDisconnectAndReportsExactIds();
    void artifactCleanupAcceptsSymlinkLeafWithoutFollowing();
    void artifactCleanupRejectsHardlinkAndSpecialLeaves_data();
    void artifactCleanupRejectsHardlinkAndSpecialLeaves();
    void artifactCleanupRejectsChangedLeafIdentity();
    void artifactCleanupBoundsPlanAndReportsRemovalFailures();
};

void CacheCleanupStoreTests::catalogCleanupEmptySuccessIsExact() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);

    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY2(plan.ok(), qPrintable(plan.result.detail));
    QVERIFY(plan.complete);
    QCOMPARE(plan.plannedFiles, 0);
    QVERIFY(plan.candidates.isEmpty());

    const auto cleaned = store.cleanupThumbnailOrphanBatch(plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QVERIFY(cleaned.complete);
    QCOMPARE(cleaned.plannedFiles, 0);
    QCOMPARE(cleaned.removedFiles, 0);
    QCOMPARE(cleaned.removedLogicalBytes, qint64{0});
}

void CacheCleanupStoreTests::
    catalogCleanupPreservesAcceptedIndexAndReportsExactBytes() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);

    const QString staged =
        QDir(temporary.path()).filePath(QStringLiteral("accepted.jpg"));
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(staged, "JPG"));
    const QByteArray stagedBytes = readFile(staged);
    const auto committed = store.commitThumbnail(
        {QStringLiteral("PASE-DEVICE-A"), remoteEntry(), staged,
         sha256(stagedBytes)});
    QVERIFY2(committed.result.ok(), qPrintable(committed.result.detail));

    CatalogStore::OriginInput origin;
    origin.deviceIdentity = QStringLiteral("PASE-DEVICE-A");
    origin.remote = remoteEntry();
    origin.sourceContentSha256 = QString(64, QLatin1Char('c'));
    origin.sourceSize = 8192;
    origin.conversionProfile = QStringLiteral("pase-h264-cleanup-test");
    origin.preparedSha256 = QString(64, QLatin1Char('d'));
    origin.operationId = canonicalId(91);
    origin.confirmedUtc = QDateTime::currentDateTimeUtc();
    const auto persistedOrigin = store.persistOrigin(origin);
    QVERIFY2(persistedOrigin.ok(), qPrintable(persistedOrigin.detail));

    const QByteArray indexBefore = readFile(store.indexPath());
    const QByteArray acceptedBefore =
        readFile(store.thumbnailPath(committed.thumbnailKey));
    const QString orphanKey(64, QLatin1Char('b'));
    const QString orphanPath =
        QDir(store.thumbnailDirectory()).filePath(orphanKey + QStringLiteral(".jpg"));
    const QByteArray orphanBytes("orphan-thumbnail");
    QVERIFY(writeFile(orphanPath, orphanBytes));

    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY2(plan.ok(), qPrintable(plan.result.detail));
    QVERIFY(plan.complete);
    QCOMPARE(plan.plannedFiles, 1);
    QCOMPARE(plan.candidates.size(), 1);

    const auto cleaned = store.cleanupThumbnailOrphanBatch(plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QVERIFY(cleaned.complete);
    QCOMPARE(cleaned.plannedFiles, 1);
    QCOMPARE(cleaned.removedFiles, 1);
    QCOMPARE(cleaned.removedLogicalBytes,
             static_cast<qint64>(orphanBytes.size()));
    QVERIFY(!QFileInfo::exists(orphanPath));
    QCOMPARE(readFile(store.indexPath()), indexBefore);
    QCOMPARE(readFile(store.thumbnailPath(committed.thumbnailKey)),
             acceptedBefore);
}

void CacheCleanupStoreTests::
    catalogCleanupFailsClosedForUnsafeCandidateAndBound() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);

    const QString outside =
        QDir(temporary.path()).filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside, QByteArrayLiteral("protected")));
    const QString unsafePath = QDir(store.thumbnailDirectory()).filePath(
        QString(64, QLatin1Char('c')) + QStringLiteral(".jpg"));
    QCOMPARE(::symlink(QFile::encodeName(outside).constData(),
                       QFile::encodeName(unsafePath).constData()),
             0);
    const auto unsafePlan = store.planThumbnailOrphanCleanup();
    QVERIFY(!unsafePlan.ok());
    QCOMPARE(unsafePlan.result.code,
             CatalogStore::ErrorCode::UnsafeCandidate);
    QCOMPARE(readFile(outside), QByteArrayLiteral("protected"));

    QVERIFY(QFile::remove(unsafePath));
    for (int index = 0; index < 3; ++index) {
        QVERIFY(writeFile(
            QDir(store.thumbnailDirectory()).filePath(
                QStringLiteral("non-candidate-%1.tmp").arg(index)),
            QByteArrayLiteral("x")));
    }
    const auto bounded = store.planThumbnailOrphanCleanup(2);
    QVERIFY(!bounded.ok());
    QCOMPARE(bounded.result.code,
             CatalogStore::ErrorCode::PlanLimitExceeded);
    QCOMPARE(bounded.plannedFiles, 0);
}

void CacheCleanupStoreTests::
    catalogCleanupRejectsHardlinkAndSpecialCandidates_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("hardlink") << QStringLiteral("hardlink");
    QTest::newRow("fifo") << QStringLiteral("fifo");
}

void CacheCleanupStoreTests::
    catalogCleanupRejectsHardlinkAndSpecialCandidates() {
    QFETCH(QString, kind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);

    const QString candidate = QDir(store.thumbnailDirectory()).filePath(
        QString(64, QLatin1Char('a')) + QStringLiteral(".jpg"));
    if (kind == QStringLiteral("hardlink")) {
        const QString outside =
            QDir(temporary.path()).filePath(QStringLiteral("outside-hardlink"));
        QVERIFY(writeFile(outside, QByteArrayLiteral("preserve")));
        QCOMPARE(::link(QFile::encodeName(outside).constData(),
                        QFile::encodeName(candidate).constData()),
                 0);
    } else {
        QCOMPARE(::mkfifo(QFile::encodeName(candidate).constData(),
                          S_IRUSR | S_IWUSR),
                 0);
    }

    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY(!plan.ok());
    QCOMPARE(plan.result.code, CatalogStore::ErrorCode::UnsafeCandidate);
    QVERIFY(QFileInfo(candidate).exists() || QFileInfo(candidate).isSymLink());
}

void CacheCleanupStoreTests::
    catalogCleanupRequiresAcceptedLoadAndPinsParentIdentity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root =
        QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    CatalogStore store(root);
    const auto beforeLoad = store.planThumbnailOrphanCleanup();
    QVERIFY(!beforeLoad.ok());
    QCOMPARE(beforeLoad.result.code, CatalogStore::ErrorCode::WritesDisabled);

    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);
    const QFileDevice::Permissions readOnlyParentPermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::ExeOther;
    QVERIFY(QFile::setPermissions(root, readOnlyParentPermissions));
    QVERIFY(QFile::setPermissions(
        store.thumbnailDirectory(), readOnlyParentPermissions));

    const QString modeCheckedKey(64, QLatin1Char('d'));
    const QString modeCheckedPath =
        QDir(store.thumbnailDirectory()).filePath(
            modeCheckedKey + QStringLiteral(".jpg"));
    QVERIFY(writeFile(modeCheckedPath, QByteArrayLiteral("mode-checked")));
    const auto allowedModePlan = store.planThumbnailOrphanCleanup();
    QVERIFY2(allowedModePlan.ok(),
             qPrintable(allowedModePlan.result.detail));
    QVERIFY(QFile::setPermissions(
        store.thumbnailDirectory(),
        readOnlyParentPermissions | QFileDevice::WriteGroup));
    const auto unsafeMode = store.cleanupThumbnailOrphanBatch(
        allowedModePlan, 0, 16);
    QVERIFY(!unsafeMode.ok());
    QCOMPARE(unsafeMode.result.code,
             CatalogStore::ErrorCode::DirectoryUnavailable);
    QCOMPARE(readFile(modeCheckedPath), QByteArrayLiteral("mode-checked"));
    QVERIFY(QFile::setPermissions(
        store.thumbnailDirectory(), readOnlyParentPermissions));
    const auto allowedModeCleanup = store.cleanupThumbnailOrphanBatch(
        allowedModePlan, 0, 16);
    QVERIFY2(allowedModeCleanup.ok(),
             qPrintable(allowedModeCleanup.result.detail));
    QVERIFY(allowedModeCleanup.complete);
    QVERIFY(!QFileInfo::exists(modeCheckedPath));

    const QString orphanKey(64, QLatin1Char('e'));
    const QString orphanPath = QDir(store.thumbnailDirectory()).filePath(
        orphanKey + QStringLiteral(".jpg"));
    QVERIFY(writeFile(orphanPath, QByteArrayLiteral("orphan")));
    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY2(plan.ok(), qPrintable(plan.result.detail));

    const QString movedDirectory =
        store.thumbnailDirectory() + QStringLiteral(".old");
    QVERIFY(QDir().rename(store.thumbnailDirectory(), movedDirectory));
    QVERIFY(QDir().mkpath(store.thumbnailDirectory()));
    const auto stale = store.cleanupThumbnailOrphanBatch(plan, 0, 16);
    QVERIFY(!stale.ok());
    QCOMPARE(stale.result.code, CatalogStore::ErrorCode::IdentityChanged);
    QVERIFY(QFileInfo::exists(
        QDir(movedDirectory).filePath(
            orphanKey + QStringLiteral(".jpg"))));

    const QString malformedRoot =
        QDir(temporary.path()).filePath(QStringLiteral("malformed-catalog"));
    QVERIFY(QDir().mkpath(malformedRoot));
    CatalogStore malformed(malformedRoot);
    QVERIFY(writeFile(malformed.indexPath(), QByteArrayLiteral("{not-json")));
    QCOMPARE(malformed.load().status,
             CatalogStore::LoadStatus::IgnoredMalformed);
    const auto rejected = malformed.planThumbnailOrphanCleanup();
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.result.code, CatalogStore::ErrorCode::WritesDisabled);
}

void CacheCleanupStoreTests::
    catalogCleanupRejectsChangedLeafIdentity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);
    const QString orphanPath = QDir(store.thumbnailDirectory()).filePath(
        QString(64, QLatin1Char('9')) + QStringLiteral(".jpg"));
    const QByteArray original("same-sized-old");
    const QByteArray replacement("same-sized-new");
    QCOMPARE(original.size(), replacement.size());
    QVERIFY(writeFile(orphanPath, original));
    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY2(plan.ok(), qPrintable(plan.result.detail));
    QVERIFY(QFile::remove(orphanPath));
    QVERIFY(writeFile(orphanPath, replacement));

    const auto stale = store.cleanupThumbnailOrphanBatch(plan, 0, 16);
    QVERIFY(!stale.ok());
    QCOMPARE(stale.result.code, CatalogStore::ErrorCode::IdentityChanged);
    QCOMPARE(stale.removedFiles, 0);
    QCOMPARE(readFile(orphanPath), replacement);
}

void CacheCleanupStoreTests::
    catalogCleanupReportsConfirmedMutationBeforeFsyncFailure() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    CatalogStore store(
        QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QCOMPARE(store.load().status, CatalogStore::LoadStatus::Empty);
    const QString orphanPath = QDir(store.thumbnailDirectory()).filePath(
        QString(64, QLatin1Char('f')) + QStringLiteral(".jpg"));
    const QByteArray payload("confirmed-before-fsync");
    QVERIFY(writeFile(orphanPath, payload));
    const auto plan = store.planThumbnailOrphanCleanup();
    QVERIFY(plan.ok());
    store.setCleanupFsyncFunctionForTesting([]() {
        errno = EIO;
        return -1;
    });
    const auto partial = store.cleanupThumbnailOrphanBatch(plan, 0, 16);
    QVERIFY(!partial.ok());
    QCOMPARE(partial.result.code, CatalogStore::ErrorCode::RemoveFailed);
    QCOMPARE(partial.removedFiles, 1);
    QCOMPARE(partial.removedLogicalBytes,
             static_cast<qint64>(payload.size()));
    QVERIFY(!QFileInfo::exists(orphanPath));
}

void CacheCleanupStoreTests::artifactCleanupEmptySuccessIsExact() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")));
    QVERIFY(store.initialize().ok());

    const auto assessment = store.cleanupAssessment();
    QVERIFY2(assessment.ok(), qPrintable(assessment.result.detail));
    QVERIFY(assessment.plan.complete);
    QCOMPARE(assessment.plan.plannedFiles, 0);
    QVERIFY(assessment.plan.candidates.isEmpty());
    const auto cleaned = store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QVERIFY(cleaned.complete);
    QCOMPARE(cleaned.plannedFiles, 0);
    QCOMPARE(cleaned.removedFiles, 0);
    QCOMPARE(cleaned.removedLogicalBytes, qint64{0});
    QVERIFY(cleaned.removedArtifactIds.isEmpty());
}

void CacheCleanupStoreTests::
    artifactCleanupRequiresExpiryAndReportsExactBytes() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QByteArray payload("expired-artifact");
    const QString artifactId = canonicalId(1);
    const auto reserved = store.reserve(
        reservation(artifactId, static_cast<quint64>(payload.size())));
    QVERIFY2(reserved.ok(), qPrintable(reserved.result.detail));
    QVERIFY(writeFile(reserved.artifact.canonicalPath, payload));
    ArtifactStore::FinalizeInput finalized;
    finalized.artifactId = artifactId;
    finalized.operationId = canonicalId(90);
    finalized.remoteName = QStringLiteral("clip.h264_2240x1080");
    finalized.outputPath = reserved.artifact.canonicalPath;
    finalized.fileSize = payload.size();
    finalized.chunkCount = 1;
    finalized.rawSha256 = sha256(payload);
    finalized.decodedSha256 = sha256(payload);
    const auto finalizedResult = store.finalize(finalized);
    QVERIFY2(finalizedResult.ok(), qPrintable(finalizedResult.detail));
    QVERIFY(store.releaseOperationHold(artifactId, canonicalId(90)).ok());

    const auto fresh = store.cleanupAssessment();
    QVERIFY(!fresh.ok());
    QCOMPARE(fresh.result.code, ArtifactStore::ErrorCode::Busy);
    QVERIFY(QFileInfo::exists(reserved.artifact.canonicalPath));

    now += ArtifactStore::kUnclaimedTtlMs - 1;
    const auto boundaryBefore = store.cleanupAssessment();
    QVERIFY(!boundaryBefore.ok());
    QCOMPARE(boundaryBefore.result.code, ArtifactStore::ErrorCode::Busy);
    ++now;
    const auto expired = store.cleanupAssessment();
    QVERIFY2(expired.ok(), qPrintable(expired.result.detail));
    QCOMPARE(expired.plan.plannedFiles, 1);
    const auto cleaned = store.cleanupBatch(expired.plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QVERIFY(cleaned.complete);
    QCOMPARE(cleaned.plannedFiles, 1);
    QCOMPARE(cleaned.removedFiles, 1);
    QCOMPARE(cleaned.removedLogicalBytes,
             static_cast<qint64>(payload.size()));
    QVERIFY(!store.contains(artifactId));
    QVERIFY(!QFileInfo::exists(reserved.artifact.canonicalPath));
}

void CacheCleanupStoreTests::
    artifactCleanupBlocksActiveLeaseAndPreservesHeldFile() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QByteArray payload("leased-artifact");
    const QString artifactId = canonicalId(2);
    const auto reserved = store.reserve(
        reservation(artifactId, static_cast<quint64>(payload.size())));
    QVERIFY(reserved.ok());
    QVERIFY(writeFile(reserved.artifact.canonicalPath, payload));
    ArtifactStore::FinalizeInput finalized;
    finalized.artifactId = artifactId;
    finalized.operationId = canonicalId(90);
    finalized.remoteName = QStringLiteral("clip.h264_2240x1080");
    finalized.outputPath = reserved.artifact.canonicalPath;
    finalized.fileSize = payload.size();
    finalized.chunkCount = 1;
    finalized.rawSha256 = sha256(payload);
    finalized.decodedSha256 = sha256(payload);
    QVERIFY(store.finalize(finalized).ok());
    QVERIFY(store.releaseOperationHold(artifactId, canonicalId(90)).ok());

    const auto claimed = store.claim(
        artifactId, canonicalId(90), QStringLiteral(":1.90"));
    QVERIFY2(claimed.ok(), qPrintable(claimed.result.detail));
    const auto assessment = store.cleanupAssessment();
    QVERIFY(!assessment.ok());
    QCOMPARE(assessment.result.code, ArtifactStore::ErrorCode::Busy);
    QVERIFY(store.contains(artifactId));
    QCOMPARE(readFile(reserved.artifact.canonicalPath), payload);
}

void CacheCleanupStoreTests::
    artifactCleanupBlocksReservationAndOperationHold() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QString reservedId = canonicalId(4);
    const auto reserved = store.reserve(reservation(reservedId, 8));
    QVERIFY2(reserved.ok(), qPrintable(reserved.result.detail));
    auto blocked = store.cleanupAssessment();
    QVERIFY(!blocked.ok());
    QCOMPARE(blocked.result.code, ArtifactStore::ErrorCode::Busy);
    QVERIFY(store.discardReservation(reservedId, canonicalId(90)).ok());

    const QString heldId = canonicalId(5);
    const QByteArray payload("operation-held-artifact");
    const auto held = makeReadyArtifact(store, heldId, payload, false);
    QVERIFY2(held.ok(), qPrintable(held.result.detail));
    now += ArtifactStore::kUnclaimedTtlMs;
    blocked = store.cleanupAssessment();
    QVERIFY(!blocked.ok());
    QCOMPARE(blocked.result.code, ArtifactStore::ErrorCode::Busy);
    QCOMPARE(readFile(held.artifact.canonicalPath), payload);
}

void CacheCleanupStoreTests::
    artifactCleanupDefersOwnerDisconnectAndReportsExactIds() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QByteArray payload("revoked-artifact");
    const QString artifactId = canonicalId(3);
    const auto reserved = store.reserve(
        reservation(artifactId, static_cast<quint64>(payload.size())));
    QVERIFY(reserved.ok());
    QVERIFY(writeFile(reserved.artifact.canonicalPath, payload));
    ArtifactStore::FinalizeInput finalized;
    finalized.artifactId = artifactId;
    finalized.operationId = canonicalId(90);
    finalized.remoteName = QStringLiteral("clip.h264_2240x1080");
    finalized.outputPath = reserved.artifact.canonicalPath;
    finalized.fileSize = payload.size();
    finalized.chunkCount = 1;
    finalized.rawSha256 = sha256(payload);
    finalized.decodedSha256 = sha256(payload);
    QVERIFY(store.finalize(finalized).ok());
    QVERIFY(store.releaseOperationHold(artifactId, canonicalId(90)).ok());

    const auto disconnected = store.ownerDisconnected(
        QStringLiteral(":1.90"),
        ArtifactStore::OwnerDisconnectMode::RevokeAndDefer);
    QVERIFY(disconnected.ok());
    QVERIFY(disconnected.removedArtifactIds.isEmpty());
    QVERIFY(store.contains(artifactId));
    QVERIFY(QFileInfo::exists(reserved.artifact.canonicalPath));

    const auto assessment = store.cleanupAssessment();
    QVERIFY2(assessment.ok(), qPrintable(assessment.result.detail));
    const auto cleaned = store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QCOMPARE(cleaned.removedArtifactIds, QStringList{artifactId});
    QCOMPARE(cleaned.ownersNoLongerUsed,
             QStringList{QStringLiteral(":1.90")});
    QCOMPARE(cleaned.removedFiles, 1);
    QCOMPARE(cleaned.removedLogicalBytes,
             static_cast<qint64>(payload.size()));
    QVERIFY(!store.contains(artifactId));
}

void CacheCleanupStoreTests::
    artifactCleanupAcceptsSymlinkLeafWithoutFollowing() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QString artifactId = canonicalId(6);
    const QByteArray payload("tracked-before-symlink");
    const auto ready = makeReadyArtifact(store, artifactId, payload);
    QVERIFY2(ready.ok(), qPrintable(ready.result.detail));
    const QString outside =
        QDir(temporary.path()).filePath(QStringLiteral("outside-sentinel"));
    QVERIFY(writeFile(outside, QByteArrayLiteral("preserve-outside")));
    QVERIFY(QFile::remove(ready.artifact.canonicalPath));
    QCOMPARE(::symlink(QFile::encodeName(outside).constData(),
                       QFile::encodeName(ready.artifact.canonicalPath).constData()),
             0);
    now += ArtifactStore::kUnclaimedTtlMs;

    const auto assessment = store.cleanupAssessment();
    QVERIFY2(assessment.ok(), qPrintable(assessment.result.detail));
    QCOMPARE(assessment.plan.plannedFiles, 1);
    QCOMPARE(assessment.plan.candidates.size(), 1);
    QCOMPARE(assessment.plan.candidates.constFirst().leaves.constFirst()
                 .logicalBytes,
             qint64{0});
    QVERIFY(assessment.plan.candidates.constFirst().leaves.constFirst()
                .symbolicLink);
    const auto cleaned = store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY2(cleaned.ok(), qPrintable(cleaned.result.detail));
    QCOMPARE(cleaned.removedFiles, 1);
    QCOMPARE(cleaned.removedLogicalBytes, qint64{0});
    QVERIFY(!store.contains(artifactId));
    QVERIFY(!QFileInfo(ready.artifact.canonicalPath).isSymLink());
    QCOMPARE(readFile(outside), QByteArrayLiteral("preserve-outside"));
}

void CacheCleanupStoreTests::
    artifactCleanupRejectsHardlinkAndSpecialLeaves_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("hardlink") << QStringLiteral("hardlink");
    QTest::newRow("fifo") << QStringLiteral("fifo");
}

void CacheCleanupStoreTests::
    artifactCleanupRejectsHardlinkAndSpecialLeaves() {
    QFETCH(QString, kind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QString artifactId = canonicalId(7);
    const QByteArray payload("tracked-before-unsafe-replacement");
    const auto ready = makeReadyArtifact(store, artifactId, payload);
    QVERIFY2(ready.ok(), qPrintable(ready.result.detail));
    QVERIFY(QFile::remove(ready.artifact.canonicalPath));
    if (kind == QStringLiteral("hardlink")) {
        const QString outside =
            QDir(temporary.path()).filePath(QStringLiteral("hardlink-source"));
        QVERIFY(writeFile(outside, payload));
        QCOMPARE(::link(QFile::encodeName(outside).constData(),
                        QFile::encodeName(ready.artifact.canonicalPath).constData()),
                 0);
    } else {
        QCOMPARE(::mkfifo(
                     QFile::encodeName(ready.artifact.canonicalPath).constData(),
                     S_IRUSR | S_IWUSR),
                 0);
    }
    now += ArtifactStore::kUnclaimedTtlMs;

    const auto assessment = store.cleanupAssessment();
    QVERIFY(!assessment.ok());
    QCOMPARE(assessment.result.code, ArtifactStore::ErrorCode::IdentityChanged);
    QVERIFY(store.contains(artifactId));
    QVERIFY(QFileInfo(ready.artifact.canonicalPath).exists());
}

void CacheCleanupStoreTests::
    artifactCleanupRejectsChangedLeafIdentity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QString artifactId = canonicalId(8);
    const QByteArray original("same-sized-old");
    const QByteArray replacement("same-sized-new");
    QCOMPARE(original.size(), replacement.size());
    const auto ready = makeReadyArtifact(store, artifactId, original);
    QVERIFY2(ready.ok(), qPrintable(ready.result.detail));
    now += ArtifactStore::kUnclaimedTtlMs;
    const auto assessment = store.cleanupAssessment();
    QVERIFY2(assessment.ok(), qPrintable(assessment.result.detail));
    QVERIFY(QFile::remove(ready.artifact.canonicalPath));
    QVERIFY(writeFile(ready.artifact.canonicalPath, replacement));

    const auto stale = store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY(!stale.ok());
    QCOMPARE(stale.result.code, ArtifactStore::ErrorCode::IdentityChanged);
    QCOMPARE(stale.removedFiles, 0);
    QVERIFY(store.contains(artifactId));
    QCOMPARE(readFile(ready.artifact.canonicalPath), replacement);

    QVERIFY(QFile::setPermissions(
        store.outboxDirectory(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup));
    const auto unsafeParent =
        store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY(!unsafeParent.ok());
    QCOMPARE(unsafeParent.result.code,
             ArtifactStore::ErrorCode::OutboxUnavailable);
    QCOMPARE(readFile(ready.artifact.canonicalPath), replacement);
}

void CacheCleanupStoreTests::
    artifactCleanupBoundsPlanAndReportsRemovalFailures() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qint64 now = 1000;
    ArtifactStore store(
        QDir(temporary.path()).filePath(QStringLiteral("outbox")),
        {}, [&now]() { return ArtifactStore::TimePoint{now}; });
    QVERIFY(store.initialize().ok());

    const QString artifactId = canonicalId(9);
    const QByteArray payload("unlink-failure-artifact");
    const auto ready = makeReadyArtifact(store, artifactId, payload);
    QVERIFY2(ready.ok(), qPrintable(ready.result.detail));
    QVERIFY(writeFile(ready.artifact.canonicalPath + QStringLiteral(".part"),
                      QByteArrayLiteral("partial")));
    now += ArtifactStore::kUnclaimedTtlMs;
    const auto bounded = store.cleanupAssessment(1);
    QVERIFY(!bounded.ok());
    QCOMPARE(bounded.result.code, ArtifactStore::ErrorCode::PlanLimitExceeded);
    QVERIFY(store.contains(artifactId));

    const auto assessment = store.cleanupAssessment();
    QVERIFY2(assessment.ok(), qPrintable(assessment.result.detail));
    QCOMPARE(assessment.plan.plannedFiles, 2);
    store.setUnlinkFunctionForTesting([](const QString &) {
        errno = EACCES;
        return -1;
    });
    const auto failed = store.cleanupBatch(assessment.plan, 0, 16);
    QVERIFY(!failed.ok());
    QCOMPARE(failed.result.code, ArtifactStore::ErrorCode::RemoveFailed);
    QCOMPARE(failed.removedFiles, 0);
    QCOMPARE(failed.removedLogicalBytes, qint64{0});
    QVERIFY(store.contains(artifactId));
    QCOMPARE(readFile(ready.artifact.canonicalPath), payload);
}

QTEST_GUILESS_MAIN(CacheCleanupStoreTests)

#include "cachecleanupstore_tests.moc"
