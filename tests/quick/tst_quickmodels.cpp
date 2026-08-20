#include <QtTest>

#include "appsettingscontroller.h"
#include "applicationpaths.h"
#include "devicemediaworkflowcontroller.h"
#include "mediacatalogmodel.h"
#include "mediaeditorcontroller.h"
#include "mediapreviewcontroller.h"
#include "mediatransform.h"
#include "operationlistmodel.h"
#include "runtimeclient.h"
#include "systemmetricsmodel.h"
#include "windowchromecontroller.h"

#include <panorama/config.hpp>

#include <QDir>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QWindow>

class QuickClientTests final : public QObject {
    Q_OBJECT

private slots:
    void sharedApplicationDataPathUsesStableManagerNamespace();
    void transformDefaultsAreCanonical();
    void runtimeMediaTargetFollowsProductId();
    void nonCropFieldsAreNeutral();
    void cropRotationResetsViewport();
    void previewUsesCanonicalTransformFilter();
    void renderedTransformPreservesDisplayGeometry();
    void previewGenerationIsDebounced();
    void restoredProtectedSourceSchedulesCurrentProfilePreview();
    void sourceSnapshotIsImmutableAfterCopy();
    void sourceSnapshotPreservesExistingFinal();
    void stageHelperRejectsSymlinkDotDotEscape();
    void protectedInboxSourceSurvivesControllerLifetime();
    void stagingTimeoutDoesNotBlockTheController();
    void drainingStageHelperBlocksRepeatedStart();
    void previewControllerRendersImmutableExactFrame();
    void publicPreviewRejectsRawDeviceH264();
    void exportHelperIsAtomicAndDoesNotClobber();
    void qmlImportScannerFindsResolvedModules();
    void catalogRejectsStaleRevision();
    void catalogResolvesThumbnailFromConfiguredDataRoot();
    void catalogExposesDeviceCopyEligibility();
    void legacyConnectionPopulatesCurrentMediaModel();
    void legacyScreenConfigKeepsManager1Shape();
    void legacyTransformBoundaryIsExplicit();
    void legacyUploadRetainsSourceUntilTerminalSignal();
    void operationsExposeStableRoles();
    void operationsRejectStaleEvents();
    void displayStateRequiresStrictlyIncreasingRevision();
    void operationAcknowledgementRequiresExactIdentity();
    void succeededOperationClearsBusyState();
    void deviceMediaWorkflowRejectsMismatchedClaimIdentity();
    void deviceMediaWorkflowSaveAsNewCompletesAndReleasesLease();
    void deviceMediaWorkflowInvalidationStopsReplaceMutations();
    void applyRequestPreservesConfirmedOverlaySettings();
    void metricsRequestUsesExplicitEnableDisableContract();
    void systemMetricsModelMapsAvailability();
    void appSettingsDefaultToEnglishAndPreserveConfig();
    void windowChromeRejectsOperationsWithoutWindow();
    void windowChromeHidesAndRestoresOnlyWithTray();

private:
    static void preparePaseDeviceMedia(
        RuntimeClient *runtime, const QString &mediaId,
        const QString &mediaName,
        const QString &deviceIdentity);
    static TryxRuntimeDeviceMediaArtifact deviceMediaArtifact(
        const QString &operationId, const QString &artifactId,
        const QString &mediaId, const QString &mediaName,
        const QString &deviceIdentity);
};

void QuickClientTests::
    sharedApplicationDataPathUsesStableManagerNamespace() {
    QCOMPARE(
        panorama::sharedApplicationDataLocation(),
        QDir(QStandardPaths::writableLocation(
                 QStandardPaths::GenericDataLocation))
            .filePath(QStringLiteral(
                "DXVSI/TRYX Panorama Manager")));
}

void QuickClientTests::preparePaseDeviceMedia(
    RuntimeClient *runtime, const QString &mediaId,
    const QString &mediaName,
    const QString &deviceIdentity) {
    runtime->serviceAvailable_ = true;
    runtime->compatible_ = true;
    runtime->connection_.revision = 1;
    runtime->connection_.printerClassConnected = true;
    runtime->connection_.printerClassDevicePresent = true;
    runtime->connection_.displaySessionActive = true;

    TryxRuntimeMediaEntry entry;
    entry.name = mediaName;
    entry.source = 1;
    entry.mediaId = mediaId;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = deviceIdentity;
    snapshot.entries = {entry};
    runtime->mediaModel()->applySnapshot(snapshot);
}

TryxRuntimeDeviceMediaArtifact
QuickClientTests::deviceMediaArtifact(
    const QString &operationId, const QString &artifactId,
    const QString &mediaId, const QString &mediaName,
    const QString &deviceIdentity) {
    TryxRuntimeDeviceMediaArtifact artifact;
    artifact.operationId = operationId;
    artifact.artifactId = artifactId;
    artifact.mediaId = mediaId;
    artifact.deviceIdentity = deviceIdentity;
    artifact.remoteName = mediaName;
    artifact.size = 1;
    artifact.decodedSha256 =
        QString(64, QLatin1Char('0'));
    artifact.localPath =
        QDir(tryxRuntimeDeviceMediaOutboxPath())
            .filePath(QStringLiteral("offline-test.h264"));
    artifact.logicalType = QStringLiteral("Video");
    artifact.leaseId = QStringLiteral("lease-1");
    artifact.leaseExpiresUtcMs =
        QDateTime::currentMSecsSinceEpoch() + 60000;
    return artifact;
}

void QuickClientTests::transformDefaultsAreCanonical() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.schemaVersion, 1U);
    QCOMPARE(transform.mode, QStringLiteral("Fit"));
    QCOMPARE(transform.rotationQuarterTurns, 0U);
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
    QCOMPARE(transform.backgroundRgb, 0U);
}

void QuickClientTests::runtimeMediaTargetFollowsProductId() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    QVERIFY(runtime.productId().isEmpty());
    QCOMPARE(runtime.mediaTargetWidth(), 2240);
    QCOMPARE(runtime.mediaTargetHeight(), 1080);
    QCOMPARE(editor.targetWidth(), 2240);
    QCOMPARE(editor.targetHeight(), 1080);

    QSignalSpy targetChanged(
        &editor, &MediaEditorController::targetChanged);
    quint64 revision = 1;
    const QStringList panoramaProductIds{
        QStringLiteral("1011"),
        QStringLiteral("391a:1021"),
        QStringLiteral("unknown"),
    };
    for (const QString &productId : panoramaProductIds) {
        TryxRuntimeSnapshot snapshot;
        snapshot.revision = revision++;
        snapshot.productId = productId;
        runtime.applyConnectionSnapshot(snapshot);
        QCOMPARE(runtime.productId(), productId);
        QCOMPARE(runtime.mediaTargetWidth(), 2240);
        QCOMPARE(runtime.mediaTargetHeight(), 1080);
        QCOMPARE(editor.targetWidth(), 2240);
        QCOMPARE(editor.targetHeight(), 1080);
    }

    TryxRuntimeSnapshot turris;
    turris.revision = revision++;
    turris.productId = QStringLiteral("391a:2011");
    runtime.applyConnectionSnapshot(turris);
    QCOMPARE(runtime.productId(), QStringLiteral("391a:2011"));
    QCOMPARE(runtime.mediaTargetWidth(), 1280);
    QCOMPARE(runtime.mediaTargetHeight(), 720);
    QCOMPARE(editor.targetWidth(), 1280);
    QCOMPARE(editor.targetHeight(), 720);

    turris.revision = revision;
    turris.productId = QStringLiteral("0x2011");
    runtime.applyConnectionSnapshot(turris);
    QCOMPARE(runtime.mediaTargetWidth(), 1280);
    QCOMPARE(runtime.mediaTargetHeight(), 720);
    QCOMPARE(targetChanged.count(), 5);
}

void QuickClientTests::nonCropFieldsAreNeutral() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(275);
    editor.setFocusX(1500);
    editor.setFocusY(8500);
    editor.setBackgroundColor(QStringLiteral("#aabbcc"));
    editor.setMode(QStringLiteral("Fill"));

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.mode, QStringLiteral("Fill"));
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
    QCOMPARE(transform.backgroundRgb, 0U);
}

void QuickClientTests::cropRotationResetsViewport() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(400);
    editor.setFocusX(0);
    editor.setFocusY(10000);
    editor.setRotation(90);

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.mode, QStringLiteral("Crop"));
    QCOMPARE(transform.rotationQuarterTurns, 1U);
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
}

void QuickClientTests::previewUsesCanonicalTransformFilter() {
    TryxRuntimeMediaTransform transform;
    transform.mode = QStringLiteral("Crop");
    transform.rotationQuarterTurns = 1;
    transform.zoomPermille = 4000;
    transform.focusX = 10000;
    transform.focusY = 0;

    const QString canonical =
        tryxMediaTransformFfmpegFilter(
            transform, kTryxMediaTargetWidth,
            kTryxMediaTargetHeight);
    QVERIFY(!canonical.isEmpty());
    QCOMPARE(
        MediaPreviewController::previewFilter(transform),
        canonical +
            QStringLiteral(",scale=1120:540:flags=lanczos"));
    const QString turrisCanonical =
        tryxMediaTransformFfmpegFilter(transform, 1280, 720);
    QVERIFY(!turrisCanonical.isEmpty());
    QCOMPARE(
        MediaPreviewController::previewFilter(
            transform, 1280, 720),
        turrisCanonical +
            QStringLiteral(",scale=640:360:flags=lanczos"));
    QVERIFY(canonical.startsWith(
        QStringLiteral("transpose=clock,")));
    QVERIFY(canonical.contains(
        QStringLiteral(
            "crop=2240:1080:"
            "'trunc((iw-2240)*10000/10000/2)*2':"
            "'trunc((ih-1080)*0/10000/2)*2'")));
    QVERIFY(turrisCanonical.contains(
        QStringLiteral(
            "crop=1280:720:"
            "'trunc((iw-1280)*10000/10000/2)*2':"
            "'trunc((ih-720)*0/10000/2)*2'")));

    for (quint32 rotation = 0; rotation < 4; ++rotation) {
        transform.rotationQuarterTurns = rotation;
        QVERIFY(!MediaPreviewController::previewFilter(
                     transform)
                     .isEmpty());
        QVERIFY(!MediaPreviewController::previewFilter(
                     transform, 1280, 720)
                     .isEmpty());
    }
}

void QuickClientTests::
    renderedTransformPreservesDisplayGeometry() {
    const QString ffmpeg = QStandardPaths::findExecutable(
        QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QSKIP("ffmpeg is optional for the unit-test environment");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto render = [&](
                            const QString &source,
                            const TryxRuntimeMediaTransform &transform,
                            const QString &outputPath) {
        QProcess process;
        process.setProgram(ffmpeg);
        process.setArguments({
            QStringLiteral("-nostdin"),
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"),
            QStringLiteral("error"),
            QStringLiteral("-y"),
            QStringLiteral("-f"),
            QStringLiteral("lavfi"),
            QStringLiteral("-i"),
            source,
            QStringLiteral("-frames:v"),
            QStringLiteral("1"),
            QStringLiteral("-vf"),
            tryxMediaTransformFfmpegFilter(transform),
            outputPath,
        });
        process.start();
        if (!process.waitForStarted(5000)) {
            return process.errorString().toUtf8();
        }
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished(5000);
            return QByteArray("ffmpeg render timed out");
        }
        const QByteArray diagnostic = process.readAll();
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0) {
            return diagnostic.isEmpty()
                ? QByteArray("ffmpeg render failed")
                : diagnostic;
        }
        return QByteArray();
    };

    TryxRuntimeMediaTransform fit =
        tryxLegacyFitMediaTransform();
    fit.backgroundRgb = 0x00FF00;
    const QString fitPath =
        QDir(directory.path()).filePath(
            QStringLiteral("anamorphic-fit.png"));
    QByteArray diagnostic = render(
        QStringLiteral(
            "color=c=red:s=720x576:d=1,setsar=64/45"),
        fit, fitPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage fitImage(fitPath);
    QVERIFY(!fitImage.isNull());
    QCOMPARE(
        fitImage.size(),
        QSize(kTryxMediaTargetWidth,
              kTryxMediaTargetHeight));
    const QColor leftPadding = fitImage.pixelColor(80, 540);
    const QColor center = fitImage.pixelColor(1120, 540);
    const QColor rightPadding = fitImage.pixelColor(2160, 540);
    QVERIFY(leftPadding.green() > 160);
    QVERIFY(leftPadding.red() < 100);
    QVERIFY(center.red() > 160);
    QVERIFY(center.green() < 100);
    QVERIFY(rightPadding.green() > 160);
    QVERIFY(rightPadding.red() < 100);

    TryxRuntimeMediaTransform rotatedFit = fit;
    rotatedFit.rotationQuarterTurns = 1;
    const QString rotatedPath =
        QDir(directory.path()).filePath(
            QStringLiteral("anamorphic-rotated-fit.png"));
    diagnostic = render(
        QStringLiteral(
            "color=c=red:s=720x576:d=1,setsar=64/45"),
        rotatedFit, rotatedPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage rotatedImage(rotatedPath);
    QVERIFY(!rotatedImage.isNull());
    const QColor rotatedPadding =
        rotatedImage.pixelColor(740, 540);
    const QColor rotatedCenter =
        rotatedImage.pixelColor(1120, 540);
    QVERIFY(rotatedPadding.green() > 160);
    QVERIFY(rotatedPadding.red() < 100);
    QVERIFY(rotatedCenter.red() > 160);
    QVERIFY(rotatedCenter.green() < 100);

    TryxRuntimeMediaTransform fill =
        tryxLegacyFitMediaTransform();
    fill.mode = QStringLiteral("Fill");
    TryxRuntimeMediaTransform neutralCrop = fill;
    neutralCrop.mode = QStringLiteral("Crop");
    QCOMPARE(
        tryxMediaTransformFfmpegFilter(fill),
        tryxMediaTransformFfmpegFilter(neutralCrop));

    const QString fillPath =
        QDir(directory.path()).filePath(
            QStringLiteral("fill.png"));
    const QString cropPath =
        QDir(directory.path()).filePath(
            QStringLiteral("neutral-crop.png"));
    diagnostic = render(
        QStringLiteral(
            "testsrc=size=333x1000:rate=1:duration=1"),
        fill, fillPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());
    diagnostic = render(
        QStringLiteral(
            "testsrc=size=333x1000:rate=1:duration=1"),
        neutralCrop, cropPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage fillImage(fillPath);
    const QImage cropImage(cropPath);
    QVERIFY(!fillImage.isNull());
    QVERIFY(!cropImage.isNull());
    QCOMPARE(fillImage, cropImage);
}

void QuickClientTests::previewGenerationIsDebounced() {
    MediaPreviewController preview;
    const quint64 initialGeneration =
        preview.requestedRenderGeneration_;

    TryxRuntimeMediaTransform transform;
    transform.mode = QStringLiteral("Fill");
    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 1);
    QVERIFY(!preview.renderDebounce_.isActive());

    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 1);

    transform.rotationQuarterTurns = 3;
    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 2);
}

void QuickClientTests::
    restoredProtectedSourceSchedulesCurrentProfilePreview() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString stagedPath =
        QDir(directory.path()).filePath(QStringLiteral("source.png"));
    QFile staged(stagedPath);
    QVERIFY(staged.open(QIODevice::WriteOnly));
    QCOMPARE(staged.write("snapshot"), qint64(8));
    staged.close();

    MediaPreviewController preview;
    preview.stagedPath_ = stagedPath;
    preview.sourceKind_ =
        MediaPreviewController::SourceKind::InboxSnapshot;
    preview.sourceProtected_ = true;
    preview.ready_ = true;

    preview.setTargetSize(1280, 720);
    QVERIFY(!preview.renderDebounce_.isActive());

    preview.restoreStagedSourceOwnership();
    QVERIFY(!preview.sourceProtected_);
    QVERIFY(preview.renderDebounce_.isActive());
}

void QuickClientTests::sourceSnapshotIsImmutableAfterCopy() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    const QString stagedPath =
        QDir(directory.path()).filePath(
            QStringLiteral("snapshot.mp4"));
    const QByteArray original("immutable-source-payload");

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    const QFileInfo sourceInfo(sourcePath);
    const MediaPreviewController::StageResult result =
        MediaPreviewController::copySourceSnapshot(
            sourcePath, stagedPath, sourceInfo.size(),
            sourceInfo.lastModified());
    QVERIFY2(result.error.isEmpty(),
             qPrintable(result.error));

    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray replacement("changed-after-preview");
    QCOMPARE(source.write(replacement), replacement.size());
    source.close();

    QFile snapshot(stagedPath);
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    QCOMPARE(snapshot.readAll(), original);
    const QFileInfo snapshotInfo(stagedPath);
    QVERIFY(!(snapshotInfo.permissions() &
              (QFileDevice::ReadGroup |
               QFileDevice::WriteGroup |
               QFileDevice::ExeGroup |
               QFileDevice::ReadOther |
               QFileDevice::WriteOther |
               QFileDevice::ExeOther)));
}

void QuickClientTests::sourceSnapshotPreservesExistingFinal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    const QString finalPath =
        QDir(directory.path()).filePath(
            QStringLiteral("existing.mp4"));
    const QByteArray sourceBytes("new-snapshot");
    const QByteArray existingBytes("must-survive");

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(sourceBytes), sourceBytes.size());
    source.close();
    QFile existing(finalPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(existingBytes), existingBytes.size());
    existing.close();

    const QFileInfo sourceInfo(sourcePath);
    const MediaPreviewController::StageResult result =
        MediaPreviewController::copySourceSnapshot(
            sourcePath, finalPath, sourceInfo.size(),
            sourceInfo.lastModified());
    QVERIFY(!result.error.isEmpty());

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), existingBytes);
    QVERIFY(!QFileInfo::exists(
        finalPath + QStringLiteral(".part")));
}

void QuickClientTests::
    stageHelperRejectsSymlinkDotDotEscape() {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (inboxPath.isEmpty()) {
        QSKIP("Qt RuntimeLocation is unavailable");
    }
    QString directoryError;
    QVERIFY2(
        MediaPreviewController::ensurePrivateDirectoryTree(
            inboxPath, &directoryError),
        qPrintable(directoryError));

    QTemporaryDir sourceDirectory;
    QVERIFY(sourceDirectory.isValid());
    const QString sourcePath =
        QDir(sourceDirectory.path()).filePath(
            QStringLiteral("source.png"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("snapshot"), qint64(8));
    source.close();

    const QString escapeParent =
        QDir(QFileInfo(inboxPath).absolutePath()).filePath(
            QStringLiteral("escape-parent"));
    const QString escapeChild =
        QDir(escapeParent).filePath(
            QStringLiteral("child"));
    QVERIFY(QDir().mkpath(escapeChild));
    const QString linkPath =
        QDir(inboxPath).filePath(
            QStringLiteral("link"));
    QVERIFY(QFile::link(escapeChild, linkPath));
    QVERIFY(QFileInfo(linkPath).isSymLink());

    const QString fileName =
        QStringLiteral("%1.png")
            .arg(QUuid::createUuid().toString(
                QUuid::WithoutBraces));
    const QString escapedPath =
        linkPath + QStringLiteral("/../") + fileName;
    const QString escapedTarget =
        QDir(escapeParent).filePath(fileName);
    QVERIFY(!MediaPreviewController::isManagedInboxPath(
        escapedPath));

    const QFileInfo sourceInfo(sourcePath);
    QCOMPARE(
        MediaPreviewController::runStageCopyHelper({
            sourcePath,
            escapedPath,
            QString::number(sourceInfo.size()),
            QString::number(
                sourceInfo.lastModified()
                    .toMSecsSinceEpoch()),
        }),
        2);
    QVERIFY(!QFileInfo::exists(escapedTarget));
}

void QuickClientTests::
    protectedInboxSourceSurvivesControllerLifetime() {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (inboxPath.isEmpty()) {
        QSKIP("Qt RuntimeLocation is unavailable");
    }
    QString directoryError;
    QVERIFY2(
        MediaPreviewController::ensurePrivateDirectoryTree(
            inboxPath, &directoryError),
        qPrintable(directoryError));
    const QString stagedPath =
        QDir(inboxPath).filePath(
            QStringLiteral("%1.png")
                .arg(QUuid::createUuid().toString(
                    QUuid::WithoutBraces)));

    QFile staged(stagedPath);
    QVERIFY(staged.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(staged.write("snapshot"), qint64(8));
    staged.close();
    QVERIFY(QFile::setPermissions(
        stagedPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    {
        MediaPreviewController preview;
        preview.stagedPath_ = stagedPath;
        preview.sourceKind_ =
            MediaPreviewController::SourceKind::InboxSnapshot;
        preview.ready_ = true;
        QVERIFY(preview.protectStagedSource());
    }
    QVERIFY(QFileInfo::exists(stagedPath));
    QVERIFY(QFile::remove(stagedPath));
}

void QuickClientTests::
    stagingTimeoutDoesNotBlockTheController() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(64, 64, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(sourcePath, "PNG"));

    const QString blockingHelper =
        QDir(directory.path()).filePath(
            QStringLiteral("blocking-helper"));
    QFile helper(blockingHelper);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    const QByteArray script("#!/bin/sh\nexec sleep 30\n");
    QCOMPARE(helper.write(script), script.size());
    helper.close();
    QVERIFY(QFile::setPermissions(
        blockingHelper,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    MediaPreviewController preview;
    preview.stageCopyProgram_ = blockingHelper;
    preview.stagingDeadline_.setInterval(50);
    const QFileInfo sourceInfo(sourcePath);
    preview.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 2000);
    QVERIFY(preview.error().contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
    QVERIFY(preview.pendingStagePath_.isEmpty());
    QVERIFY(preview.stageProcess_ == nullptr);

    const QString inboxPath = tryxRuntimeMediaInboxPath();
    QCOMPARE(
        QDir(inboxPath).entryList(
            QDir::Files | QDir::NoDotAndDotDot),
        QStringList{});
    QTRY_VERIFY_WITH_TIMEOUT(
        !MediaPreviewController::
            stageHelperDrainInProgress(),
        2000);
}

void QuickClientTests::
    drainingStageHelperBlocksRepeatedStart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(64, 64, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(sourcePath, "PNG"));

    const QString blockingHelper =
        QDir(directory.path()).filePath(
            QStringLiteral("blocking-helper"));
    QFile helper(blockingHelper);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    const QByteArray script("#!/bin/sh\nexec sleep 30\n");
    QCOMPARE(helper.write(script), script.size());
    helper.close();
    QVERIFY(QFile::setPermissions(
        blockingHelper,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    MediaPreviewController first;
    first.stageCopyProgram_ = blockingHelper;
    const QFileInfo sourceInfo(sourcePath);
    first.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QTRY_VERIFY_WITH_TIMEOUT(
        first.stageProcess_ != nullptr &&
            first.stageProcess_->state() ==
                QProcess::Running,
        2000);
    first.cancel();
    QVERIFY(
        MediaPreviewController::
            stageHelperDrainInProgress());

    MediaPreviewController second;
    second.stageCopyProgram_ = blockingHelper;
    second.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QVERIFY(second.stageProcess_ == nullptr);
    QVERIFY(second.error().contains(
        QStringLiteral("still stopping"),
        Qt::CaseInsensitive));
    second.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QVERIFY(second.stageProcess_ == nullptr);
    QVERIFY(second.error().contains(
        QStringLiteral("still stopping"),
        Qt::CaseInsensitive));

    QTRY_VERIFY_WITH_TIMEOUT(
        !MediaPreviewController::
            stageHelperDrainInProgress(),
        2000);
}

void QuickClientTests::
    previewControllerRendersImmutableExactFrame() {
    if (QStandardPaths::findExecutable(
            QStringLiteral("ffmpeg")).isEmpty()) {
        QSKIP("ffmpeg is optional for the unit-test environment");
    }

    QTemporaryDir sourceDirectory;
    QVERIFY(sourceDirectory.isValid());
    const QString sourcePath =
        QDir(sourceDirectory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(320, 240, QImage::Format_RGB32);
    source.fill(QColor(QStringLiteral("#dd2211")));
    QVERIFY(source.save(sourcePath, "PNG"));

    QString stagedPath;
    QString firstPreviewPath;
    QString secondPreviewPath;
    {
        MediaPreviewController preview;
        preview.load(QUrl::fromLocalFile(sourcePath));
        QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 30000);
        QVERIFY2(preview.ready(), qPrintable(preview.error()));
        stagedPath = preview.sourcePath();
        firstPreviewPath =
            preview.previewUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(stagedPath));

        QImage firstPreview(firstPreviewPath);
        QVERIFY(!firstPreview.isNull());
        QCOMPARE(firstPreview.size(), QSize(1120, 540));
        const QColor firstCenter =
            firstPreview.pixelColor(
                firstPreview.width() / 2,
                firstPreview.height() / 2);
        QVERIFY(firstCenter.red() > 180);
        QVERIFY(firstCenter.blue() < 80);

        QImage replacement(320, 240, QImage::Format_RGB32);
        replacement.fill(QColor(QStringLiteral("#1144dd")));
        QVERIFY(replacement.save(sourcePath, "PNG"));

        TryxRuntimeMediaTransform fill;
        fill.mode = QStringLiteral("Fill");
        preview.setTransform(fill);
        QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 30000);
        QVERIFY2(preview.ready(), qPrintable(preview.error()));
        secondPreviewPath =
            preview.previewUrl().toLocalFile();
        QVERIFY(firstPreviewPath != secondPreviewPath);

        QImage secondPreview(secondPreviewPath);
        QVERIFY(!secondPreview.isNull());
        QCOMPARE(secondPreview.size(), QSize(1120, 540));
        const QColor secondCenter =
            secondPreview.pixelColor(
                secondPreview.width() / 2,
                secondPreview.height() / 2);
        QVERIFY(secondCenter.red() > 180);
        QVERIFY(secondCenter.blue() < 80);
    }
    QVERIFY(!QFileInfo::exists(stagedPath));
    QVERIFY(!QFileInfo::exists(firstPreviewPath));
    QVERIFY(!QFileInfo::exists(secondPreviewPath));
}

void QuickClientTests::publicPreviewRejectsRawDeviceH264() {
    QVERIFY(!MediaPreviewController::isSupportedSuffix(
        QStringLiteral("h264")));
    QVERIFY(MediaPreviewController::isSupportedSuffix(
        QStringLiteral("mp4")));
}

void QuickClientTests::
    exportHelperIsAtomicAndDoesNotClobber() {
    const QString outbox =
        tryxRuntimeDeviceMediaOutboxPath();
    QVERIFY(!outbox.isEmpty());
    QVERIFY(QDir().mkpath(outbox));
    QVERIFY(QFile::setPermissions(
        outbox,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString sourcePath =
        QDir(outbox).filePath(
            QUuid::createUuid().toString(
                QUuid::WithoutBraces) +
            QStringLiteral(".h264"));
    const QByteArray payload(
        "validated recovered H264 test bytes");
    QFile source(sourcePath);
    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(source.write(payload),
             static_cast<qint64>(payload.size()));
    source.close();
    QVERIFY(QFile::setPermissions(
        sourcePath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner));

    QTemporaryDir destinationDirectory;
    QVERIFY(destinationDirectory.isValid());
    const QString destinationPath =
        QDir(destinationDirectory.path()).filePath(
            QStringLiteral("export.h264"));
    const QString expectedSha =
        QString::fromLatin1(
            QCryptographicHash::hash(
                payload, QCryptographicHash::Sha256)
                .toHex());
    const QStringList arguments{
        sourcePath,
        destinationPath,
        QString::number(payload.size()),
        expectedSha,
        QStringLiteral("0"),
    };

    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            arguments),
        0);
    QFile exported(destinationPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), payload);
    exported.close();

    QVERIFY(exported.open(
        QIODevice::WriteOnly |
        QIODevice::Truncate));
    QCOMPARE(exported.write("keep"), qint64(4));
    exported.close();
    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            arguments),
        3);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), QByteArray("keep"));
    exported.close();

    QStringList overwriteArguments = arguments;
    overwriteArguments[4] = QStringLiteral("1");
    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            overwriteArguments),
        0);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), payload);
    exported.close();
    QVERIFY(QFile::remove(sourcePath));
}

void QuickClientTests::qmlImportScannerFindsResolvedModules() {
    const QString scanner = QString::fromLocal8Bit(
        qgetenv("TRYX_QMLIMPORTSCANNER"));
    const QString qmlRoot = QString::fromLocal8Bit(
        qgetenv("TRYX_QML_ROOT"));
    const QString importPath = QString::fromLocal8Bit(
        qgetenv("TRYX_QML_IMPORT_PATH"));
    if (scanner.isEmpty() || qmlRoot.isEmpty() ||
        importPath.isEmpty()) {
        QFAIL(
            "qmlimportscanner paths are required; run make quick-check");
    }

    QProcess process;
    process.setProgram(scanner);
    process.setArguments({
        QStringLiteral("-rootPath"), qmlRoot,
        QStringLiteral("-importPath"), importPath,
    });
    process.start();
    QVERIFY2(
        process.waitForStarted(5000),
        qPrintable(process.errorString()));
    QVERIFY2(
        process.waitForFinished(30000),
        "qmlimportscanner did not finish within 30 seconds");
    const QByteArray standardError =
        process.readAllStandardError();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(
        process.exitCode() == 0,
        standardError.constData());

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            process.readAllStandardOutput(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    const QJsonArray imports = document.array();
    QVERIFY2(!imports.isEmpty(),
             "qmlimportscanner returned no imports");

    const QSet<QString> requiredModules{
        QStringLiteral("Qt.labs.folderlistmodel"),
        QStringLiteral("QtQuick"),
        QStringLiteral("QtQuick.Controls"),
        QStringLiteral("QtQuick.Controls.Material"),
        QStringLiteral("QtQuick.Dialogs"),
        QStringLiteral("QtQuick.Layouts"),
    };
    QSet<QString> resolvedRequiredModules;
    for (const QJsonValue &value : imports) {
        QVERIFY(value.isObject());
        const QJsonObject import = value.toObject();
        const QString type =
            import.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("module") &&
            type != QStringLiteral("directory")) {
            continue;
        }
        const QString name =
            import.value(QStringLiteral("name"))
                .toString()
                .trimmed();
        const QString path =
            import.value(QStringLiteral("path"))
                .toString()
                .trimmed();
        QVERIFY2(
            !name.isEmpty(),
            "qmlimportscanner returned an unnamed module or directory");
        if (type == QStringLiteral("module") &&
            requiredModules.contains(name)) {
            QVERIFY2(
                !path.isEmpty(),
                qPrintable(
                    QStringLiteral(
                        "qmlimportscanner did not resolve required module %1")
                        .arg(name)));
            resolvedRequiredModules.insert(name);
        }
    }
    for (const QString &module : requiredModules) {
        QVERIFY2(
            resolvedRequiredModules.contains(module),
            qPrintable(
                QStringLiteral(
                    "qmlimportscanner omitted required module %1")
                    .arg(module)));
    }
}

void QuickClientTests::catalogRejectsStaleRevision() {
    MediaCatalogModel model;
    TryxRuntimeMediaCatalogSnapshot newer;
    newer.revision = 5;
    newer.deviceIdentity = QStringLiteral("device-a");
    TryxRuntimeMediaEntry entry;
    entry.name = QStringLiteral("new.mp4.h264_2240x1080");
    newer.entries.append(entry);
    model.applySnapshot(newer);

    TryxRuntimeMediaCatalogSnapshot stale;
    stale.revision = 4;
    stale.deviceIdentity = QStringLiteral("device-b");
    model.applySnapshot(stale);

    QCOMPARE(model.revision(), 5U);
    QCOMPARE(model.deviceIdentity(), QStringLiteral("device-a"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(
        model.data(model.index(0), MediaCatalogModel::NameRole)
            .toString(),
        entry.name);
}

void QuickClientTests::
    catalogResolvesThumbnailFromConfiguredDataRoot() {
    QTemporaryDir temporaryData;
    QVERIFY(temporaryData.isValid());

    const QString thumbnailKey(64, QLatin1Char('a'));
    const QString thumbnailDirectory =
        QDir(temporaryData.path()).filePath(
            QStringLiteral("media-catalog/thumbnails"));
    QVERIFY(QDir().mkpath(thumbnailDirectory));
    const QString thumbnailPath =
        QDir(thumbnailDirectory).filePath(
            thumbnailKey + QStringLiteral(".jpg"));
    QFile thumbnail(thumbnailPath);
    QVERIFY(thumbnail.open(QIODevice::WriteOnly));
    QCOMPARE(thumbnail.write("thumbnail"), 9);
    thumbnail.close();

    TryxRuntimeMediaEntry entry;
    entry.name =
        QStringLiteral("user.mp4.h264_2240x1080");
    entry.thumbnailKey = thumbnailKey;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = QStringLiteral("device-a");
    snapshot.entries = {entry};

    MediaCatalogModel model(temporaryData.path());
    model.applySnapshot(snapshot);

    QCOMPARE(
        model.data(
            model.index(0),
            MediaCatalogModel::ThumbnailUrlRole).toUrl(),
        QUrl::fromLocalFile(thumbnailPath));
}

void QuickClientTests::catalogExposesDeviceCopyEligibility() {
    MediaCatalogModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(
        roles.value(MediaCatalogModel::MediaIdRole),
        QByteArray("mediaId"));
    QCOMPARE(
        roles.value(MediaCatalogModel::DeviceCopyAllowedRole),
        QByteArray("deviceCopyAllowed"));
    QCOMPARE(
        roles.value(MediaCatalogModel::DeviceCopyBlockReasonRole),
        QByteArray("deviceCopyBlockReason"));

    TryxRuntimeMediaEntry userMedia;
    userMedia.name =
        QStringLiteral("user.mp4.h264_2240x1080");
    userMedia.source = 1;
    userMedia.mediaId = QStringLiteral("media-user");

    TryxRuntimeMediaEntry presetMedia;
    presetMedia.name =
        QStringLiteral("preset.mp4.h264_2240x1080");
    presetMedia.source = 0;
    presetMedia.mediaId = QStringLiteral("media-preset");
    presetMedia.readOnly = true;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = QStringLiteral("device-a");
    snapshot.entries = {userMedia, presetMedia};
    model.applySnapshot(snapshot);

    QVERIFY(model.canStageDeviceCopy(userMedia.mediaId));
    QVERIFY(model.data(
        model.index(0),
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(!model.canStageDeviceCopy(presetMedia.mediaId));
    QVERIFY(!model.data(
        model.index(1),
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(!model.deviceCopyBlockReason(
        presetMedia.mediaId).isEmpty());
    QVERIFY(!model.canStageDeviceCopy(QStringLiteral("missing")));
}

void QuickClientTests::
    legacyConnectionPopulatesCurrentMediaModel() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;

    TryxRuntimeSnapshot snapshot;
    snapshot.revision = 4;
    snapshot.connected = true;
    snapshot.serial = QStringLiteral("LEGACY-1");
    snapshot.mediaFiles = {
        QStringLiteral("first.mp4"),
        QStringLiteral("first.mp4"),
        QString(),
        QStringLiteral("second.gif"),
    };
    runtime.applyConnectionSnapshot(snapshot);

    QVERIFY(runtime.ready());
    QVERIFY(runtime.legacyConnected());
    QVERIFY(runtime.connectionStatus().contains(
        QStringLiteral("Legacy")));
    QCOMPARE(runtime.mediaModel()->rowCount(), 2);
    QCOMPARE(
        runtime.mediaModel()->deviceIdentity(),
        QStringLiteral("legacy:LEGACY-1"));
    const QModelIndex first =
        runtime.mediaModel()->index(0, 0);
    QVERIFY(first.data(
        MediaCatalogModel::DeleteAllowedRole).toBool());
    QVERIFY(!first.data(
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(first.data(
        MediaCatalogModel::MediaIdRole).toString().isEmpty());
}

void QuickClientTests::
    legacyScreenConfigKeepsManager1Shape() {
    TryxRuntimeApplyRequest request;
    request.media = {
        QStringLiteral("left.mp4"),
        QStringLiteral("right.mp4"),
    };
    request.ratio = QStringLiteral("2:1");
    request.screenMode =
        QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    request.sysinfoLabels =
        {QStringLiteral("CPU Temperature")};
    request.settingsPosition = QStringLiteral("Top");
    request.settingsColor = QStringLiteral("#dcdcdc");
    request.settingsAlign = QStringLiteral("Left");
    request.settingsBadges =
        {QStringLiteral("CPU Badge")};
    request.filterOpacity = 12;
    request.presetId = QStringLiteral("custom");
    request.sysinfoLabels2 =
        {QStringLiteral("GPU Temperature")};
    request.settingsBadges2 =
        {QStringLiteral("GPU Badge")};
    request.waterfallMode = true;

    const QVariantList arguments =
        RuntimeClient::legacyScreenConfigArguments(request);
    QCOMPARE(arguments.size(), 14);
    QCOMPARE(arguments.at(0).toStringList(), request.media);
    QCOMPARE(arguments.at(2).toString(), request.screenMode);
    QCOMPARE(arguments.at(9).toInt(), request.filterOpacity);
    QCOMPARE(
        arguments.at(11).toStringList(),
        request.sysinfoLabels2);
    QCOMPARE(arguments.at(13).toBool(), true);
}

void QuickClientTests::legacyTransformBoundaryIsExplicit() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.revision = 1;
    runtime.connection_.connected = true;

    TryxRuntimeMediaTransform transform =
        tryxLegacyFitMediaTransform();
    transform.mode = QStringLiteral("Fill");
    const QString operationId =
        runtime.queueUploadWithTransform(
            QStringLiteral("/tmp/source.mp4"), transform);
    QVERIFY(operationId.isEmpty());
    QVERIFY(runtime.diagnostic().contains(
        QStringLiteral("default Fit")));
    QVERIFY(!runtime.operationBusy());
}

void QuickClientTests::
    legacyUploadRetainsSourceUntilTerminalSignal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    QFile source(sourcePath);
    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(source.write("legacy-upload"), qint64(13));
    source.close();
    QVERIFY(QFile::setPermissions(
        sourcePath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner));

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.revision = 1;
    runtime.connection_.connected = true;

    QString claimError;
    const QString operationId =
        QStringLiteral("legacy-operation");
    QVERIFY(runtime.claimLegacyUploadSource(
        operationId, sourcePath, &claimError));
    QVERIFY(claimError.isEmpty());
    const QString claimedPath =
        runtime.legacyUpload_.claimedPath;
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));
    QVERIFY(runtime.operationBusy());

    QSignalSpy rejected(
        &runtime,
        &RuntimeClient::operationRequestRejected);
    QSignalSpy accepted(
        &runtime,
        &RuntimeClient::operationRequestAccepted);
    runtime.onLegacyUploadTimeout();
    QCOMPARE(rejected.count(), 1);
    QVERIFY(runtime.operationBusy());
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));

    QVERIFY(QFile::remove(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));
    runtime.onLegacyMediaUploaded(
        QStringLiteral("uploaded.mp4"), 2);
    QVERIFY(!runtime.operationBusy());
    QVERIFY(!QFileInfo::exists(claimedPath));
    QCOMPARE(accepted.count(), 0);
}

void QuickClientTests::operationsExposeStableRoles() {
    OperationListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.value(OperationListModel::StateRole),
             QByteArray("operationState"));
    QCOMPARE(roles.value(OperationListModel::ProgressRole),
             QByteArray("progress"));

    TryxRuntimeOperationsSnapshot snapshot;
    snapshot.revision = 1;
    TryxRuntimeOperationInfo info;
    info.id = QStringLiteral("operation");
    info.state = QStringLiteral("Uploading");
    info.completed = 25;
    info.total = 100;
    snapshot.operations.append(info);
    model.applySnapshot(snapshot);

    QCOMPARE(
        model.data(model.index(0), OperationListModel::ProgressRole)
            .toDouble(),
        0.25);
    QVERIFY(!model.data(model.index(0),
                        OperationListModel::TerminalRole)
                 .toBool());
}

void QuickClientTests::operationsRejectStaleEvents() {
    OperationListModel model;
    TryxRuntimeOperationInfo current;
    current.id = QStringLiteral("current");
    current.state = QStringLiteral("Uploading");
    QVERIFY(model.upsert(current, 10));

    TryxRuntimeOperationInfo stale = current;
    stale.state = QStringLiteral("Failed");
    QVERIFY(!model.upsert(stale, 9));
    QCOMPARE(
        model.data(model.index(0), OperationListModel::StateRole)
            .toString(),
        QStringLiteral("Uploading"));

    QVERIFY(!model.remove(current.id, 9));
    QCOMPARE(model.rowCount(), 1);

    TryxRuntimeOperationsSnapshot staleSnapshot;
    staleSnapshot.revision = 8;
    QVERIFY(!model.applySnapshot(staleSnapshot));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.revision(), 10U);
}

void QuickClientTests::
    displayStateRequiresStrictlyIncreasingRevision() {
    RuntimeClient runtime(true);
    runtime.compatible_ = true;
    QSignalSpy displaySpy(
        &runtime, &RuntimeClient::displayChanged);

    TryxRuntimeDisplayState initial;
    initial.revision = 0;
    initial.valid = true;
    initial.brightness = 41;
    runtime.onDisplayStateUpdated(initial);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 41);

    TryxRuntimeDisplayState duplicate = initial;
    duplicate.brightness = 99;
    runtime.onDisplayStateUpdated(duplicate);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 41);

    TryxRuntimeDisplayState newer = initial;
    newer.revision = 1;
    newer.brightness = 73;
    runtime.onDisplayStateUpdated(newer);
    QCOMPARE(displaySpy.count(), 2);
    QCOMPARE(runtime.brightness(), 73);

    runtime.onDisplayStateUpdated(initial);
    QCOMPARE(displaySpy.count(), 2);
    QCOMPARE(runtime.brightness(), 73);

    runtime.clearRuntimeState();
    runtime.compatible_ = true;
    displaySpy.clear();
    TryxRuntimeDisplayState afterRestart = initial;
    afterRestart.brightness = 64;
    runtime.onDisplayStateUpdated(afterRestart);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 64);

    RuntimeClient legacyRuntime(true);
    legacyRuntime.compatible_ = true;
    legacyRuntime.connection_.connected = true;
    legacyRuntime.onLegacyBrightnessChanged(55, 5);
    QCOMPARE(legacyRuntime.brightness(), 55);

    TryxRuntimeDisplayState staleAfterLegacy;
    staleAfterLegacy.revision = 4;
    staleAfterLegacy.valid = true;
    staleAfterLegacy.brightness = 99;
    legacyRuntime.onDisplayStateUpdated(staleAfterLegacy);
    QCOMPARE(legacyRuntime.brightness(), 55);
}

void QuickClientTests::
    operationAcknowledgementRequiresExactIdentity() {
    const QString expected =
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111");
    TryxRuntimeOperationInfo observed;

    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected, expected, observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        expected,
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222"),
        observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        expected, QString(), observed));

    observed.id = expected;
    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected, QString(), observed));
    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected,
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222"),
        observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        QString(), QString(), observed));
}

void QuickClientTests::succeededOperationClearsBusyState() {
    RuntimeClient runtime(true);
    runtime.compatible_ = true;
    TryxRuntimeOperationInfo operation;
    operation.id = QStringLiteral("operation");
    operation.state = QStringLiteral("Uploading");

    QVERIFY(QMetaObject::invokeMethod(
        &runtime, "onOperationChanged", Qt::DirectConnection,
        Q_ARG(TryxRuntimeOperationInfo, operation),
        Q_ARG(quint64, 1)));
    QVERIFY(runtime.operationBusy());

    operation.state = QStringLiteral("Succeeded");
    QVERIFY(QMetaObject::invokeMethod(
        &runtime, "onOperationChanged", Qt::DirectConnection,
        Q_ARG(TryxRuntimeOperationInfo, operation),
        Q_ARG(quint64, 2)));
    QVERIFY(!runtime.operationBusy());
    QVERIFY(OperationListModel::isTerminal(operation));
}

void QuickClientTests::
    deviceMediaWorkflowRejectsMismatchedClaimIdentity() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const QString stageOperationId =
        runtime.offlineRequests_.constFirst().operationId;
    QVERIFY(!stageOperationId.isEmpty());

    TryxRuntimeOperationInfo staged;
    staged.id = stageOperationId;
    staged.kind = QStringLiteral("StageDeviceMedia");
    staged.state = QStringLiteral("Succeeded");
    staged.resultName = artifactId;
    emit runtime.operationUpdated(staged);

    QCOMPARE(runtime.offlineRequests_.size(), 2);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("ClaimDeviceMediaArtifact"));

    TryxRuntimeDeviceMediaArtifact stale =
        deviceMediaArtifact(
            QStringLiteral("stale-operation"), artifactId,
            mediaId, mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        stageOperationId, stale);

    QCOMPARE(runtime.offlineRequests_.size(), 3);
    const RuntimeClient::OfflineRequest release =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        release.method,
        QStringLiteral("ReleaseDeviceMediaArtifact"));
    QCOMPARE(
        release.arguments,
        QVariantList({artifactId, stale.leaseId}));
    QVERIFY(!workflow.busy());
    QVERIFY(!workflow.error().isEmpty());
    QVERIFY(!editor.recoveredDeviceCopy());

    const int requestCount =
        runtime.offlineRequests_.size();
    emit runtime.artifactClaimed(
        stageOperationId,
        deviceMediaArtifact(
            stageOperationId, artifactId, mediaId,
            mediaName, deviceIdentity));
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    deviceMediaWorkflowSaveAsNewCompletesAndReleasesLease() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest stageRequest =
        runtime.offlineRequests_.constFirst();
    QCOMPARE(
        stageRequest.method,
        QStringLiteral("QueueStageDeviceMedia"));
    QCOMPARE(
        stageRequest.arguments.at(1).toString(), mediaId);
    QVERIFY(workflow.busy());

    TryxRuntimeOperationInfo staleStage;
    staleStage.id = QStringLiteral("stale-stage");
    staleStage.kind = QStringLiteral("StageDeviceMedia");
    staleStage.state = QStringLiteral("Succeeded");
    staleStage.resultName = artifactId;
    emit runtime.operationUpdated(staleStage);
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    TryxRuntimeOperationInfo staged = staleStage;
    staged.id = stageRequest.operationId;
    emit runtime.operationUpdated(staged);
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    const RuntimeClient::OfflineRequest claimRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        claimRequest.method,
        QStringLiteral("ClaimDeviceMediaArtifact"));
    QCOMPARE(
        claimRequest.arguments,
        QVariantList(
            {stageRequest.operationId, artifactId}));

    const TryxRuntimeDeviceMediaArtifact artifact =
        deviceMediaArtifact(
            stageRequest.operationId, artifactId, mediaId,
            mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        QStringLiteral("stale-stage"), artifact);
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    QVERIFY(workflow.claimPending_);

    emit runtime.artifactClaimed(
        stageRequest.operationId, artifact);
    QVERIFY(editor.recoveredDeviceCopy());
    QVERIFY(!workflow.claimPending_);

    QVERIFY(QMetaObject::invokeMethod(
        &workflow.renewTimer_, "timeout",
        Qt::DirectConnection));
    QCOMPARE(runtime.offlineRequests_.size(), 3);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("RenewDeviceMediaArtifactLease"));
    QCOMPARE(
        runtime.offlineRequests_.constLast().arguments,
        QVariantList({artifactId, artifact.leaseId}));

    emit editor.recoveredSaveAsNewRequested(
        editor.transform());
    QCOMPARE(runtime.offlineRequests_.size(), 4);
    const RuntimeClient::OfflineRequest mutationRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        mutationRequest.method,
        QStringLiteral(
            "QueueRecoveredMediaUploadWithTransform"));
    QCOMPARE(
        mutationRequest.arguments.at(1).toString(),
        artifactId);
    QCOMPARE(
        mutationRequest.arguments.at(2).toString(),
        artifact.leaseId);
    QVERIFY(editor.submissionPending());

    TryxRuntimeOperationInfo staleMutation;
    staleMutation.id = QStringLiteral("stale-mutation");
    staleMutation.kind =
        QStringLiteral("RecoveredMediaUpload");
    staleMutation.state = QStringLiteral("Succeeded");
    emit runtime.operationUpdated(staleMutation);
    QVERIFY(editor.submissionPending());
    QCOMPARE(runtime.offlineRequests_.size(), 4);

    TryxRuntimeOperationInfo completed = staleMutation;
    completed.id = mutationRequest.operationId;
    emit runtime.operationUpdated(completed);

    QCOMPARE(runtime.offlineRequests_.size(), 5);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("ReleaseDeviceMediaArtifact"));
    QCOMPARE(
        runtime.offlineRequests_.constLast().arguments,
        QVariantList({artifactId, artifact.leaseId}));
    QVERIFY(!workflow.busy());
    QVERIFY(!editor.submissionPending());
    QVERIFY(!editor.recoveredDeviceCopy());

    const int requestCount =
        runtime.offlineRequests_.size();
    emit runtime.operationUpdated(completed);
    emit editor.recoveredSaveAsNewRequested(
        editor.transform());
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    deviceMediaWorkflowInvalidationStopsReplaceMutations() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    const QString stageOperationId =
        runtime.offlineRequests_.constFirst().operationId;

    TryxRuntimeOperationInfo staged;
    staged.id = stageOperationId;
    staged.kind = QStringLiteral("StageDeviceMedia");
    staged.state = QStringLiteral("Succeeded");
    staged.resultName = artifactId;
    emit runtime.operationUpdated(staged);

    const TryxRuntimeDeviceMediaArtifact artifact =
        deviceMediaArtifact(
            stageOperationId, artifactId, mediaId,
            mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        stageOperationId, artifact);
    QVERIFY(editor.recoveredDeviceCopy());

    emit editor.recoveredReplaceRequested(
        editor.transform());
    QCOMPARE(runtime.offlineRequests_.size(), 3);
    const RuntimeClient::OfflineRequest replaceRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        replaceRequest.method,
        QStringLiteral("QueueReplaceDeviceMedia"));
    QVERIFY(editor.submissionPending());

    const quint64 serviceEpoch = runtime.serviceEpoch_;
    const int requestCount =
        runtime.offlineRequests_.size();
    runtime.onServiceUnregistered(
        tryxRuntimeServiceName());

    QCOMPARE(runtime.serviceEpoch_, serviceEpoch + 1);
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
    QVERIFY(!workflow.busy());
    QVERIFY(workflow.error().contains(
        QStringLiteral("runtime"),
        Qt::CaseInsensitive));
    QVERIFY(!editor.submissionPending());
    QVERIFY(!editor.recoveredDeviceCopy());

    TryxRuntimeOperationInfo completed;
    completed.id = replaceRequest.operationId;
    completed.kind = QStringLiteral("ReplaceDeviceMedia");
    completed.state = QStringLiteral("Succeeded");
    completed.terminalOutcome = QStringLiteral("Replaced");
    emit runtime.operationUpdated(completed);
    emit runtime.artifactClaimed(
        stageOperationId, artifact);
    emit editor.recoveredReplaceRequested(
        editor.transform());
    QVERIFY(QMetaObject::invokeMethod(
        &workflow.renewTimer_, "timeout",
        Qt::DirectConnection));
    workflow.beginEdit(mediaId, mediaName);

    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    applyRequestPreservesConfirmedOverlaySettings() {
    RuntimeClient runtime(true);
    TryxRuntimeDisplayState state;
    state.revision = 1;
    state.valid = true;
    state.settingsPosition = QStringLiteral("Bottom");
    state.settingsColor = QStringLiteral("#123456");
    state.settingsAlign = QStringLiteral("Center");
    state.settingsBadges = {
        QStringLiteral("CPU Badge")};
    state.settingsPosition2 = QStringLiteral("Top");
    state.settingsColor2 = QStringLiteral("#654321");
    state.settingsAlign2 = QStringLiteral("Right");
    state.settingsBadges2 = {
        QStringLiteral("GPU Badge")};
    runtime.applyDisplayState(state);

    const TryxRuntimeApplyRequest full =
        runtime.fullScreenApplyRequest(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"),
            {QStringLiteral("CPU Temperature")},
            {QStringLiteral("GPU Badge")});
    QCOMPARE(
        full.settingsBadges,
        QStringList{QStringLiteral("GPU Badge")});
    QCOMPARE(full.settingsPosition, state.settingsPosition);
    QCOMPARE(full.settingsColor, state.settingsColor);
    QCOMPARE(full.settingsAlign, state.settingsAlign);

    const TryxRuntimeApplyRequest split =
        runtime.splitScreenApplyRequest(
            QStringLiteral("left.mp4.h264_2240x1080"),
            QStringLiteral("right.mp4.h264_2240x1080"),
            {QStringLiteral("CPU Temperature")},
            {QStringLiteral("GPU Temperature")},
            {QStringLiteral("GPU Badge")},
            {QStringLiteral("CPU Badge")});
    QCOMPARE(
        split.settingsBadges,
        QStringList{QStringLiteral("GPU Badge")});
    QCOMPARE(
        split.settingsBadges2,
        QStringList{QStringLiteral("CPU Badge")});
    QCOMPARE(split.settingsPosition2, state.settingsPosition2);
    QCOMPARE(split.settingsColor2, state.settingsColor2);
    QCOMPARE(split.settingsAlign2, state.settingsAlign2);

    QString badgeError;
    QVERIFY(runtime.badgesSelectionValid(
        {QStringLiteral("CPU Badge"),
         QStringLiteral("GPU Badge")},
        &badgeError));
    QVERIFY(!runtime.badgesSelectionValid(
        {QStringLiteral("Unsupported Badge")},
        &badgeError));
    QVERIFY(!badgeError.isEmpty());
}

void QuickClientTests::
    metricsRequestUsesExplicitEnableDisableContract() {
    RuntimeClient runtime(true);
    TryxRuntimeMetricsState state;
    state.revision = 1;
    state.availableMetrics = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("GPU Temperature")};
    state.alignment = QStringLiteral("Right");
    state.textColor = 0x00123456;
    runtime.applyMetricsState(state);

    TryxRuntimeMetricsConfigRequest request;
    QString error;
    QVERIFY(runtime.metricsConfigRequest(
        false,
        {QStringLiteral("CPU Temperature")},
        QStringLiteral("invalid"),
        QStringLiteral("not-a-color"),
        &request, &error));
    QVERIFY(request.metrics.isEmpty());
    QVERIFY(!request.enabled);
    QCOMPARE(request.alignment, state.alignment);
    QCOMPARE(request.textColor, state.textColor);

    QVERIFY(!runtime.metricsConfigRequest(
        true, {}, QStringLiteral("Left"),
        QStringLiteral("#dcdcdc"), &request, &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(runtime.metricsConfigRequest(
        true,
        {QStringLiteral("CPU Temperature")},
        QStringLiteral("Center"),
        QStringLiteral("#abcdef"),
        &request, &error));
    QVERIFY(request.enabled);
    QCOMPARE(
        request.metrics,
        QStringList{QStringLiteral("CPU Temperature")});
    QCOMPARE(request.alignment, QStringLiteral("Center"));
    QCOMPARE(request.textColor, 0x00abcdefU);
}

void QuickClientTests::systemMetricsModelMapsAvailability() {
    SystemMetricsModel model(false, nullptr);
    QSignalSpy changed(
        &model, &SystemMetricsModel::metricsChanged);

    SystemMetrics sample;
    sample.cpu.usagePercent = 17.5;
    sample.cpu.usageAvailable = true;
    sample.cpu.temperature = 54.0;
    sample.cpu.temperatureAvailable = true;
    sample.cpu.frequencyMHz = 4250.0;
    sample.cpu.frequencyAvailable = true;

    GpuMetrics gpu;
    gpu.name = QStringLiteral("Primary GPU");
    gpu.usagePercent = 42.0;
    gpu.usageAvailable = true;
    gpu.temperature = 61.0;
    gpu.temperatureAvailable = true;
    gpu.frequencyMHz = 2300.0;
    gpu.frequencyAvailable = true;
    gpu.vramUsedMB = 4096;
    gpu.vramTotalMB = 8192;
    sample.gpus.append(gpu);

    sample.ram.usagePercent = 33.0;
    sample.ram.usageAvailable = true;
    sample.ram.usedMB = 10240;
    sample.ram.totalMB = 32768;
    sample.disk.usagePercent = 58.0;
    sample.disk.usageAvailable = true;
    sample.disk.usedGB = 580;
    sample.disk.totalGB = 1000;
    sample.net.available = true;
    sample.net.rxSpeedKBs = 125.5;
    sample.net.txSpeedKBs = 12.25;

    model.applyMetrics(sample);

    QCOMPARE(changed.count(), 1);
    QVERIFY(model.sampled());
    QVERIFY(model.cpuUsageAvailable());
    QCOMPARE(model.cpuUsage(), 17.5);
    QCOMPARE(model.cpuTemperature(), 54.0);
    QVERIFY(model.gpuPresent());
    QCOMPARE(model.gpuName(), QStringLiteral("Primary GPU"));
    QCOMPARE(model.gpuVramUsedMB(), 4096);
    QVERIFY(model.gpuVramAvailable());
    QCOMPARE(model.ramTotalMB(), 32768);
    QVERIFY(model.diskUsageAvailable());
    QCOMPARE(model.diskTotalGB(), 1000);
    QVERIFY(model.networkAvailable());
    QCOMPARE(model.rxSpeedKBs(), 125.5);

    sample.gpus.clear();
    model.applyMetrics(sample);
    QVERIFY(!model.gpuPresent());
    QVERIFY(!model.gpuUsageAvailable());
    QCOMPARE(model.gpuName(), QString());
}

void QuickClientTests::
    appSettingsDefaultToEnglishAndPreserveConfig() {
    QTemporaryDir configRoot;
    QVERIFY(configRoot.isValid());

    const bool hadConfigHome =
        qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray previousConfigHome =
        qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME",
            QFile::encodeName(configRoot.path()));

    {
        AppSettingsController settings(true);
        QCOMPARE(settings.language(), QStringLiteral("en"));
        QVERIFY(settings.errorMessage().isEmpty());
    }

    panorama::Config config;
    config.port = "ttyACM-test";
    config.brightness = 61;
    config.keepalive_interval = 23;
    config.language = "ru";
    config.pase_overlay_lease_mode =
        "ping-and-overlay-lease";
    QVERIFY(panorama::ConfigManager::save_config(config));

    {
        AppSettingsController settings(true);
        QCOMPARE(settings.language(), QStringLiteral("ru"));
        QCOMPARE(
            settings.devicePort(),
            QString::fromStdString(config.port));
        QCOMPARE(
            settings.keepaliveInterval(),
            config.keepalive_interval);
        QSignalSpy languageChanged(
            &settings,
            &AppSettingsController::languageChanged);
        QSignalSpy deviceSettingsChanged(
            &settings,
            &AppSettingsController::deviceSettingsChanged);
        settings.setLanguage(QStringLiteral("en"));
        settings.setDevicePort(
            QStringLiteral("/dev/ttyACM9"));
        settings.setKeepaliveInterval(41);
        QCOMPARE(settings.language(), QStringLiteral("en"));
        QCOMPARE(
            settings.devicePort(),
            QStringLiteral("/dev/ttyACM9"));
        QCOMPARE(settings.keepaliveInterval(), 41);
        QCOMPARE(languageChanged.count(), 1);
        QCOMPARE(deviceSettingsChanged.count(), 2);
        QVERIFY(settings.errorMessage().isEmpty());
    }

    const auto saved =
        panorama::ConfigManager::load_config();
    QVERIFY(saved.has_value());
    QCOMPARE(saved->language, std::string("en"));
    QCOMPARE(saved->port, std::string("/dev/ttyACM9"));
    QCOMPARE(saved->brightness, config.brightness);
    QCOMPARE(saved->keepalive_interval, 41);
    QCOMPARE(saved->pase_overlay_lease_mode,
             config.pase_overlay_lease_mode);

    if (hadConfigHome) {
        qputenv("XDG_CONFIG_HOME", previousConfigHome);
    } else {
        qunsetenv("XDG_CONFIG_HOME");
    }
}

void QuickClientTests::
    windowChromeRejectsOperationsWithoutWindow() {
    WindowChromeController chrome;
    QVERIFY(!chrome.ready());
    QVERIFY(!chrome.maximized());
    QVERIFY(!chrome.trayAvailable());
    QVERIFY(!chrome.hiddenToTray());
    QVERIFY(!chrome.startMove());
    QVERIFY(!chrome.startResize(Qt::LeftEdge));
    QVERIFY(!chrome.startResize(
        Qt::LeftEdge | Qt::RightEdge));
    QVERIFY(!chrome.handleCloseRequest());
    chrome.setTrayAvailable(true);
    QVERIFY(chrome.trayAvailable());
    QVERIFY(!chrome.handleCloseRequest());
    QVERIFY(!chrome.hiddenToTray());
    chrome.showWindow();
    chrome.setTrayAvailable(false);
    QVERIFY(!chrome.trayAvailable());
    chrome.minimize();
    chrome.toggleMaximized();
    chrome.closeWindow();
}

void QuickClientTests::
    windowChromeHidesAndRestoresOnlyWithTray() {
    QWindow window;
    window.resize(640, 480);
    window.show();
    QTRY_VERIFY(window.isVisible());

    WindowChromeController chrome;
    chrome.setWindow(&window);
    QVERIFY(chrome.ready());
    QVERIFY(!chrome.handleCloseRequest());
    QVERIFY(window.isVisible());

    chrome.setTrayAvailable(true);
    QVERIFY(chrome.handleCloseRequest());
    QVERIFY(chrome.hiddenToTray());
    QVERIFY(!window.isVisible());

    chrome.showWindow();
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());

    QVERIFY(chrome.handleCloseRequest());
    QVERIFY(chrome.hiddenToTray());
    chrome.setTrayAvailable(false);
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());
}

int main(int argc, char **argv) {
    if (argc > 1 &&
        qstrcmp(argv[1], "--internal-stage-copy") == 0) {
        QStringList helperArguments;
        for (int index = 2; index < argc; ++index) {
            helperArguments.append(
                QString::fromLocal8Bit(argv[index]));
        }
        return MediaPreviewController::runStageCopyHelper(
            helperArguments);
    }

    QTemporaryDir isolatedRuntime(
        QDir(QDir::tempPath()).filePath(
            QStringLiteral(
                "tryx-quick-tests-runtime-XXXXXX")));
    if (!isolatedRuntime.isValid() ||
        !QFile::setPermissions(
            isolatedRuntime.path(),
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        return 2;
    }
    qputenv(
        "XDG_RUNTIME_DIR",
        QFile::encodeName(isolatedRuntime.path()));

    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    QuickClientTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_quickmodels.moc"
