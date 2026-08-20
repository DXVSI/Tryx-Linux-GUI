#include "printermediapreparer.h"

#include "mediatransform.h"
#include "printermediapreparersupport_p.h"
#include "printerprotocol.h"

#include <panorama/media.hpp>

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>

#include <optional>
#include <utility>

namespace {

constexpr int kMediaPreparationDeadlineMs = 15 * 60 * 1000;
constexpr int kThumbnailPreparationDeadlineMs = 2 * 60 * 1000;
const qint64 kMediaPreparationOutputCapBytes =
    tryx::printer_media_preparer_support::kMaxRetryCacheBytes +
    1024LL * 1024LL;

}  // namespace

using tryx::printer_media_preparer_support::SafeSourceHashResult;
using tryx::printer_media_preparer_support::TurrisMediaBlobResult;
using tryx::printer_media_preparer_support::generatedPrinterMediaName;
using tryx::printer_media_preparer_support::h264PrinterName;
using tryx::printer_media_preparer_support::hashRegularSourceFile;
using tryx::printer_media_preparer_support::isSha256Hex;
using tryx::printer_media_preparer_support::kMaxRetryCacheBytes;
using tryx::printer_media_preparer_support::kTurrisImageKind;
using tryx::printer_media_preparer_support::kTurrisMediaHeight;
using tryx::printer_media_preparer_support::kTurrisMediaWidth;
using tryx::printer_media_preparer_support::kTurrisProductId;
using tryx::printer_media_preparer_support::kTurrisVideoKind;
using tryx::printer_media_preparer_support::printerConversionProfile;
using tryx::printer_media_preparer_support::printerTempPath;
using tryx::printer_media_preparer_support::sha256File;
using tryx::printer_media_preparer_support::writeTurrisMediaBlob;

PrinterMediaPreparer::PrinterMediaPreparer(QObject *parent)
    : QObject(parent),
      process_(new QProcess(this)),
      processDeadlineTimer_(new QTimer(this)) {
    processDeadlineTimer_->setSingleShot(true);
    connect(processDeadlineTimer_, &QTimer::timeout, this, [this]() {
        if (!active_ || process_->state() == QProcess::NotRunning) {
            return;
        }
        preparationTimedOut_ = true;
        process_->kill();
    });
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, [this]() {
        processOutput_.append(process_->readAll());
        constexpr qsizetype kMaxDiagnosticBytes = 8192;
        if (processOutput_.size() > kMaxDiagnosticBytes) {
            processOutput_ = processOutput_.right(kMaxDiagnosticBytes);
        }
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                finishPreparation(exitCode, status == QProcess::NormalExit);
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (active_ && error == QProcess::FailedToStart) {
                    finishPreparation(-1, false);
                }
            });
}

PrinterMediaPreparer::~PrinterMediaPreparer() {
    shutdown();
}

void PrinterMediaPreparer::analyzeSource(
    const QString &operationId, const QString &localPath,
    quint64 generation,
    const TryxRuntimeMediaTransform &transform,
    quint16 productId) {
    const auto isCancelled = [this, operationId, generation]() {
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != generation) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(operationId);
    };
    const QString profile = printerConversionProfile(
        localPath, transform, productId);
    if (profile.isEmpty()) {
        emit failed(operationId,
                    tryxMediaTransformIsValid(transform)
                        ? tr("Unsupported media file type")
                        : tr("Media transform is invalid"),
                    generation);
        return;
    }
    const SafeSourceHashResult result =
        hashRegularSourceFile(localPath, isCancelled);
    if (result.cancelled) {
        return;
    }
    if (!isSha256Hex(result.sha256) || result.size <= 0) {
        emit failed(
            operationId,
            result.error.isEmpty()
                ? tr("Could not calculate the source media content hash")
                : result.error,
            generation);
        return;
    }
    emit sourceAnalyzed(operationId, localPath, result.sha256,
                        result.size, profile, generation);
}

void PrinterMediaPreparer::cancelRetryValidation(
    const QString &validationId) {
    if (validationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&retryValidationMutex_);
    cancelledRetryValidations_.insert(validationId);
}

void PrinterMediaPreparer::clearRetryValidationCancellation(
    const QString &validationId) {
    QMutexLocker locker(&retryValidationMutex_);
    cancelledRetryValidations_.remove(validationId);
}

void PrinterMediaPreparer::requestOperationCancellation(
    const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&preparationCancellationMutex_);
    cancelledPreparationOperations_.insert(operationId);
}

void PrinterMediaPreparer::requestGenerationCancellation(
    quint64 currentGeneration) {
    quint64 observed = preparationGenerationGate_.load(
        std::memory_order_acquire);
    while (observed < currentGeneration &&
           !preparationGenerationGate_.compare_exchange_weak(
               observed, currentGeneration, std::memory_order_release,
               std::memory_order_acquire)) {
    }
}

void PrinterMediaPreparer::prepare(const QString &operationId,
                                   const QString &devicePath,
                                   const QString &localPath,
                                   const QString &expectedSourceSha256,
                                   quint64 generation,
                                   const TryxRuntimeMediaTransform &transform,
                                   quint16 productId) {
    if (shuttingDown_) {
        return;
    }
    if (active_) {
        pendingOperationId_ = operationId;
        pendingDevicePath_ = devicePath;
        pendingLocalPath_ = localPath;
        pendingExpectedSourceSha256_ = expectedSourceSha256;
        pendingTransform_ = transform;
        pendingRecoveredVideo_ = false;
        pendingProductId_ = productId;
        pendingGeneration_ = generation;
        hasPending_ = true;
        cancelling_ = true;
        process_->kill();
        return;
    }
    startPreparation(operationId, devicePath, localPath,
                     expectedSourceSha256, generation, transform, false,
                     productId);
}

void PrinterMediaPreparer::prepareRecovered(
    const QString &operationId, const QString &devicePath,
    const QString &localPath, const QString &expectedSourceSha256,
    quint64 generation, const TryxRuntimeMediaTransform &transform,
    quint16 productId) {
    if (shuttingDown_) {
        return;
    }
    if (active_) {
        pendingOperationId_ = operationId;
        pendingDevicePath_ = devicePath;
        pendingLocalPath_ = localPath;
        pendingExpectedSourceSha256_ = expectedSourceSha256;
        pendingTransform_ = transform;
        pendingRecoveredVideo_ = true;
        pendingProductId_ = productId;
        pendingGeneration_ = generation;
        hasPending_ = true;
        cancelling_ = true;
        process_->kill();
        return;
    }
    startPreparation(operationId, devicePath, localPath,
                     expectedSourceSha256, generation, transform, true,
                     productId);
}

void PrinterMediaPreparer::startPreparation(const QString &operationId,
                                            const QString &devicePath,
                                            const QString &localPath,
                                            const QString &expectedSourceSha256,
                                            quint64 generation,
                                            const TryxRuntimeMediaTransform &transform,
                                            bool recoveredVideo,
                                            quint16 productId) {
    const auto isCancelled = [this, operationId, generation]() {
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != generation) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(operationId);
    };
    if (isCancelled()) {
        {
            QMutexLocker locker(&preparationCancellationMutex_);
            cancelledPreparationOperations_.remove(operationId);
        }
        emit failed(operationId,
                    tr("Media preparation was cancelled before it started"),
                    generation);
        return;
    }
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(productId);
    if (!productProfile || !productProfile->mediaUploadSupported) {
        emit failed(
            operationId,
            tr("Media upload is not supported for USB product %1")
                .arg(printerProductIdString(productId)),
            generation);
        return;
    }
    const QFileInfo inputInfo(localPath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        emit failed(operationId, tr("Media file does not exist"), generation);
        return;
    }
    if (!expectedSourceSha256.isEmpty() &&
        !isSha256Hex(expectedSourceSha256)) {
        emit failed(operationId,
                    tr("Source media content identity is invalid"),
                    generation);
        return;
    }
    QString transformError;
    if (!tryxMediaTransformIsValid(transform, &transformError)) {
        emit failed(operationId,
                    tr("Media transform is invalid: %1")
                        .arg(transformError),
                    generation);
        return;
    }

    const auto type = recoveredVideo
        ? panorama::MediaType::Video
        : panorama::Media::detect_type(localPath.toStdString());
    QString baseExtension;
    switch (type) {
    case panorama::MediaType::Image:
        baseExtension = QStringLiteral("png");
        break;
    case panorama::MediaType::Video:
        baseExtension = QStringLiteral("mp4");
        break;
    case panorama::MediaType::Gif:
        baseExtension = QStringLiteral("gif");
        break;
    case panorama::MediaType::Unknown:
        emit failed(operationId, tr("Unsupported media file type"), generation);
        return;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        emit failed(operationId,
                    tr("ffmpeg not found. Install it with your system package manager"),
                    generation);
        return;
    }
    const bool turrisMedia = productId == kTurrisProductId;
    if (turrisMedia &&
        QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
        emit failed(operationId,
                    tr("ffprobe is required to prepare Turris media"),
                    generation);
        return;
    }

    const QString remoteBaseName = generatedPrinterMediaName(baseExtension);
    const QString remoteName = h264PrinterName(remoteBaseName, productId);
    const QString outputPath = printerTempPath(remoteName);
    const QString rawMediaPath = turrisMedia
        ? outputPath + QStringLiteral(".raw.h264")
        : QString();
    const QString mediaOutputPath = turrisMedia
        ? rawMediaPath
        : outputPath;
    const QString stagedThumbnailPath = outputPath + QStringLiteral(".jpg");
    const QString stagedThumbnailTempPath =
        outputPath + QStringLiteral(".part.jpg");
    QFile::remove(outputPath);
    QFile::remove(rawMediaPath);
    QFile::remove(stagedThumbnailPath);
    QFile::remove(stagedThumbnailTempPath);

    QStringList arguments{QStringLiteral("-y")};
    if (type == panorama::MediaType::Image) {
        arguments << QStringLiteral("-loop") << QStringLiteral("1")
                  << QStringLiteral("-framerate") << QStringLiteral("30");
        if (!turrisMedia) {
            arguments << QStringLiteral("-t") << QStringLiteral("60");
        }
    } else if (recoveredVideo) {
        arguments << QStringLiteral("-f") << QStringLiteral("h264")
                  << QStringLiteral("-framerate") << QStringLiteral("30");
    }
    const QString filter = tryxMediaTransformFfmpegFilter(
        transform, productProfile->mediaWidth,
        productProfile->mediaHeight);
    if (filter.isEmpty()) {
        emit failed(operationId, tr("Media transform filter is invalid"),
                    generation);
        return;
    }
    arguments << QStringLiteral("-i") << localPath
              << QStringLiteral("-c:v") << QStringLiteral("libx264");
    if (turrisMedia) {
        const bool turrisImage = type == panorama::MediaType::Image;
        const QString turrisFilter = filter + QStringLiteral(
            ",scale=in_range=auto:out_range=full:out_color_matrix=bt709");
        arguments << QStringLiteral("-preset")
                  << (turrisImage ? QStringLiteral("medium")
                                  : QStringLiteral("fast"));
        if (turrisImage) {
            arguments << QStringLiteral("-crf") << QStringLiteral("18");
        } else {
            arguments << QStringLiteral("-b:v") << QStringLiteral("12M")
                      << QStringLiteral("-maxrate") << QStringLiteral("12M")
                      << QStringLiteral("-bufsize") << QStringLiteral("24M");
        }
        arguments << QStringLiteral("-vf") << turrisFilter
                  << QStringLiteral("-an")
                  << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
                  << QStringLiteral("-r") << QStringLiteral("30")
                  << QStringLiteral("-fps_mode") << QStringLiteral("cfr")
                  << QStringLiteral("-profile:v") << QStringLiteral("main")
                  << QStringLiteral("-level:v")
                  << (turrisImage
                          ? QStringLiteral("4.0")
                          : QStringLiteral("4.1"))
                  << QStringLiteral("-g")
                  << (turrisImage ? QStringLiteral("30")
                                  : QStringLiteral("60"))
                  << QStringLiteral("-keyint_min")
                  << (turrisImage ? QStringLiteral("30")
                                  : QStringLiteral("60"))
                  << QStringLiteral("-sc_threshold") << QStringLiteral("0")
                  << QStringLiteral("-bf") << QStringLiteral("0")
                  << QStringLiteral("-flags") << QStringLiteral("+cgop")
                  << QStringLiteral("-color_range") << QStringLiteral("pc")
                  << QStringLiteral("-colorspace") << QStringLiteral("bt709")
                  << QStringLiteral("-color_primaries") << QStringLiteral("bt709")
                  << QStringLiteral("-color_trc") << QStringLiteral("bt709")
                  << QStringLiteral("-x264-params")
                  << QStringLiteral(
                         "aud=1:repeat-headers=1:open-gop=0:force-cfr=1:fullrange=on:colorprim=bt709:transfer=bt709:colormatrix=bt709");
        if (turrisImage) {
            arguments << QStringLiteral("-frames:v") << QStringLiteral("1");
        }
    } else {
        arguments << QStringLiteral("-preset") << QStringLiteral("veryfast")
                  << QStringLiteral("-crf") << QStringLiteral("23")
                  << QStringLiteral("-vf") << filter
                  << QStringLiteral("-an");
    }
    arguments << QStringLiteral("-f") << QStringLiteral("h264")
              << QStringLiteral("-fs")
              << QString::number(kMediaPreparationOutputCapBytes)
              << mediaOutputPath;

    operationId_ = operationId;
    devicePath_ = devicePath;
    sourcePath_ = localPath;
    uploadPath_ = outputPath;
    rawMediaPath_ = rawMediaPath;
    remoteName_ = remoteName;
    stagedThumbnailTempPath_ = stagedThumbnailTempPath;
    stagedThumbnailPath_ = stagedThumbnailPath;
    stagedThumbnailSha256_.clear();
    preparedSha256_.clear();
    expectedSourceSha256_ = expectedSourceSha256;
    transform_ = transform;
    recoveredVideo_ = recoveredVideo;
    productId_ = productId;
    turrisMediaKind_ = type == panorama::MediaType::Image
        ? kTurrisImageKind
        : kTurrisVideoKind;
    generation_ = generation;
    processOutput_.clear();
    cancelling_ = false;
    preparationTimedOut_ = false;
    mediaPreparationDeadline_ =
        QDeadlineTimer(kMediaPreparationDeadlineMs);
    active_ = true;
    phase_ = PreparationPhase::Media;
    emit progress(operationId,
                  tr("Converting to printer-class H264..."), generation);
    process_->setProgram(ffmpeg);
    process_->setArguments(arguments);
    process_->start();
    processDeadlineTimer_->start(kMediaPreparationDeadlineMs);
}

void PrinterMediaPreparer::finishPreparation(int exitCode, bool normalExit) {
    if (!active_) {
        return;
    }
    processDeadlineTimer_->stop();
    processOutput_.append(process_->readAll());
    if (phase_ == PreparationPhase::Media) {
        finishMediaPreparation(exitCode, normalExit);
    } else if (phase_ == PreparationPhase::Thumbnail) {
        finishThumbnailPreparation(exitCode, normalExit);
    } else if (phase_ == PreparationPhase::FrameCount) {
        finishTurrisFrameCountPreparation(exitCode, normalExit);
    }
}

void PrinterMediaPreparer::finishMediaPreparation(int exitCode,
                                                  bool normalExit) {
    if (preparationTimedOut_) {
        failPreparation(tr("Conversion to printer-class H264 timed out"),
                        false);
        return;
    }
    const bool cancelled = cancelling_ || shuttingDown_;
    const QString encodedMediaPath = productId_ == kTurrisProductId
        ? rawMediaPath_
        : uploadPath_;
    const QFileInfo outputInfo(encodedMediaPath);
    if (!cancelled && outputInfo.exists() && outputInfo.isFile() &&
        outputInfo.size() > kMaxRetryCacheBytes) {
        failPreparation(
            tr("Prepared H264 exceeds the supported upload size"), false);
        return;
    }
    const bool outputReady = !cancelled && normalExit && exitCode == 0 &&
                             outputInfo.exists() && outputInfo.isFile() &&
                             outputInfo.size() > 0;
    if (!outputReady) {
        QString detail = QString::fromLocal8Bit(processOutput_).trimmed();
        if (detail.size() > 1000) {
            detail = detail.right(1000);
        }
        failPreparation(
            detail.isEmpty()
                ? tr("Conversion to printer-class H264 failed")
                : tr("Conversion to printer-class H264 failed: %1").arg(detail),
            cancelled);
        return;
    }

    const QString activeOperationId = operationId_;
    const quint64 activeGeneration = generation_;
    const auto hashCancelled = [this, activeOperationId,
                                activeGeneration]() {
        if (mediaPreparationDeadline_.hasExpired()) {
            return true;
        }
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != activeGeneration) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(activeOperationId);
    };
    if (productId_ != kTurrisProductId) {
        preparedSha256_ = sha256File(uploadPath_, hashCancelled);
        if (preparedSha256_.isEmpty()) {
            if (mediaPreparationDeadline_.hasExpired()) {
                failPreparation(
                    tr("Conversion to printer-class H264 timed out"), false);
                return;
            }
            if (hashCancelled()) {
                failPreparation(QString(), true);
                return;
            }
            failPreparation(tr("Could not verify the prepared H264 file"), false);
            return;
        }
    }

    if (!expectedSourceSha256_.isEmpty()) {
        const SafeSourceHashResult sourceResult =
            hashRegularSourceFile(sourcePath_, hashCancelled);
        if (sourceResult.cancelled) {
            failPreparation(QString(), true);
            return;
        }
        if (!isSha256Hex(sourceResult.sha256) ||
            sourceResult.sha256 != expectedSourceSha256_) {
            failPreparation(
                sourceResult.error.isEmpty()
                    ? tr("Source media changed after content analysis")
                    : sourceResult.error,
                false);
            return;
        }
    }

    processOutput_.clear();
    preparationTimedOut_ = false;
    phase_ = PreparationPhase::Thumbnail;
    emit progress(operationId_, tr("Preparing a persistent preview..."),
                  generation_);
    process_->setProgram(
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
    process_->setArguments(
        {QStringLiteral("-y"), QStringLiteral("-f"),
         QStringLiteral("h264"), QStringLiteral("-framerate"),
         QStringLiteral("30"), QStringLiteral("-i"), encodedMediaPath,
         QStringLiteral("-vf"), QStringLiteral("scale=384:-2"),
         QStringLiteral("-frames:v"), QStringLiteral("1"),
         QStringLiteral("-q:v"), QStringLiteral("4"),
         stagedThumbnailTempPath_});
    process_->start();
    processDeadlineTimer_->start(kThumbnailPreparationDeadlineMs);
}

void PrinterMediaPreparer::finishThumbnailPreparation(int exitCode,
                                                      bool normalExit) {
    if (preparationTimedOut_) {
        QFile::remove(stagedThumbnailTempPath_);
        QFile::remove(stagedThumbnailPath_);
        emit progress(
            operationId_,
            tr("Persistent preview timed out; continuing with a placeholder"),
            generation_);
        if (productId_ == kTurrisProductId) {
            startTurrisFrameCountPreparation(QString());
        } else {
            completePreparation(QString());
        }
        return;
    }
    const bool cancelled = cancelling_ || shuttingDown_;
    if (cancelled) {
        failPreparation(QString(), true);
        return;
    }

    QString thumbnailSha256;
    const QFileInfo thumbnailInfo(stagedThumbnailTempPath_);
    if (normalExit && exitCode == 0 && thumbnailInfo.exists() &&
        thumbnailInfo.isFile() && thumbnailInfo.size() > 0) {
        QImageReader reader(stagedThumbnailTempPath_);
        if (reader.canRead()) {
            thumbnailSha256 = sha256File(stagedThumbnailTempPath_);
        }
    }
    if (thumbnailSha256.isEmpty() ||
        !QFile::rename(stagedThumbnailTempPath_, stagedThumbnailPath_)) {
        thumbnailSha256.clear();
        QFile::remove(stagedThumbnailTempPath_);
        QFile::remove(stagedThumbnailPath_);
    }
    if (productId_ == kTurrisProductId) {
        startTurrisFrameCountPreparation(thumbnailSha256);
    } else {
        completePreparation(thumbnailSha256);
    }
}

void PrinterMediaPreparer::startTurrisFrameCountPreparation(
    const QString &thumbnailSha256) {
    if (mediaPreparationDeadline_.hasExpired()) {
        failPreparation(tr("Turris media preparation timed out"), false);
        return;
    }
    const QString ffprobe =
        QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) {
        failPreparation(tr("ffprobe is required to prepare Turris media"),
                        false);
        return;
    }
    const qint64 remainingMs = mediaPreparationDeadline_.remainingTime();
    if (remainingMs <= 0) {
        failPreparation(tr("Turris media preparation timed out"), false);
        return;
    }

    stagedThumbnailSha256_ = thumbnailSha256;
    processOutput_.clear();
    preparationTimedOut_ = false;
    phase_ = PreparationPhase::FrameCount;
    emit progress(operationId_,
                  tr("Counting exact Turris media frames..."), generation_);
    process_->setProgram(ffprobe);
    process_->setArguments(
        {QStringLiteral("-v"), QStringLiteral("error"),
         QStringLiteral("-f"), QStringLiteral("h264"),
         QStringLiteral("-select_streams"), QStringLiteral("v:0"),
         QStringLiteral("-count_frames"), QStringLiteral("-show_entries"),
         QStringLiteral("stream=width,height,nb_read_frames"),
         QStringLiteral("-of"),
         QStringLiteral("default=noprint_wrappers=1"),
         rawMediaPath_});
    process_->start();
    processDeadlineTimer_->start(static_cast<int>(remainingMs));
}

void PrinterMediaPreparer::finishTurrisFrameCountPreparation(
    int exitCode, bool normalExit) {
    if (preparationTimedOut_) {
        failPreparation(tr("Turris frame counting timed out"), false);
        return;
    }
    const bool cancelled = cancelling_ || shuttingDown_;
    if (cancelled) {
        failPreparation(QString(), true);
        return;
    }
    QHash<QString, QString> probeValues;
    QString probeText = QString::fromLatin1(processOutput_);
    probeText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList probeLines =
        probeText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    bool probeShapeValid = true;
    for (const QString &rawLine : probeLines) {
        const QString line = rawLine.trimmed();
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            probeShapeValid = false;
            break;
        }
        const QString key = line.left(separator);
        if ((key != QStringLiteral("width") &&
             key != QStringLiteral("height") &&
             key != QStringLiteral("nb_read_frames")) ||
            probeValues.contains(key)) {
            probeShapeValid = false;
            break;
        }
        probeValues.insert(key, line.mid(separator + 1));
    }
    bool frameCountOk = false;
    const quint64 frameCount =
        probeValues.value(QStringLiteral("nb_read_frames")).toULongLong(
            &frameCountOk);
    const bool geometryValid =
        probeValues.value(QStringLiteral("width")) ==
            QString::number(kTurrisMediaWidth) &&
        probeValues.value(QStringLiteral("height")) ==
            QString::number(kTurrisMediaHeight);
    if (!normalExit || exitCode != 0 || !probeShapeValid ||
        probeValues.size() != 3 || !geometryValid || !frameCountOk ||
        frameCount == 0 || frameCount > 0xffffffffULL ||
        (turrisMediaKind_ == kTurrisImageKind && frameCount != 1)) {
        QString detail = QString::fromLocal8Bit(processOutput_).trimmed();
        if (detail.size() > 1000) {
            detail = detail.right(1000);
        }
        failPreparation(
            detail.isEmpty()
                ? tr("Could not determine the exact Turris frame count")
                : tr("Could not determine the exact Turris frame count: %1")
                      .arg(detail),
            false);
        return;
    }

    const QString activeOperationId = operationId_;
    const quint64 activeGeneration = generation_;
    const auto isCancelled = [this, activeOperationId,
                              activeGeneration]() {
        if (mediaPreparationDeadline_.hasExpired()) {
            return true;
        }
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != activeGeneration) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(activeOperationId);
    };
    emit progress(operationId_, tr("Finalizing Turris media blob..."),
                  generation_);
    const TurrisMediaBlobResult blob = writeTurrisMediaBlob(
        rawMediaPath_, uploadPath_, turrisMediaKind_, frameCount,
        isCancelled);
    if (blob.sha256.isEmpty()) {
        if (mediaPreparationDeadline_.hasExpired()) {
            failPreparation(tr("Turris media preparation timed out"), false);
        } else if (blob.cancelled || isCancelled()) {
            failPreparation(QString(), true);
        } else {
            failPreparation(
                blob.error.isEmpty()
                    ? tr("Could not finalize the Turris media blob")
                    : blob.error,
                false);
        }
        return;
    }
    preparedSha256_ = blob.sha256;
    QFile::remove(rawMediaPath_);
    completePreparation(stagedThumbnailSha256_);
}

void PrinterMediaPreparer::completePreparation(
    const QString &thumbnailSha256) {
    const QString operationId = operationId_;
    const QString devicePath = devicePath_;
    const QString sourcePath = sourcePath_;
    const QString outputPath = uploadPath_;
    const QString remoteName = remoteName_;
    const QString preparedSha256 = preparedSha256_;
    const QString thumbnailPath = thumbnailSha256.isEmpty()
        ? QString()
        : stagedThumbnailPath_;
    const quint64 generation = generation_;

    deliveredPaths_.insert(outputPath);
    if (!thumbnailPath.isEmpty()) {
        deliveredPaths_.insert(thumbnailPath);
    }
    resetPreparationState();
    emit prepared(operationId, devicePath, sourcePath, outputPath,
                  remoteName, preparedSha256, thumbnailPath,
                  thumbnailSha256, generation);
    startPendingIfAvailable();
}

void PrinterMediaPreparer::failPreparation(const QString &message,
                                           bool cancelled) {
    const QString operationId = operationId_;
    const QString outputPath = uploadPath_;
    const QString rawMediaPath = rawMediaPath_;
    const QString thumbnailPath = stagedThumbnailPath_;
    const quint64 generation = generation_;
    if (!outputPath.isEmpty()) {
        QFile::remove(outputPath);
    }
    if (!rawMediaPath.isEmpty()) {
        QFile::remove(rawMediaPath);
    }
    if (!stagedThumbnailTempPath_.isEmpty()) {
        QFile::remove(stagedThumbnailTempPath_);
    }
    if (!thumbnailPath.isEmpty()) {
        QFile::remove(thumbnailPath);
    }
    resetPreparationState();
    if (!cancelled) {
        emit failed(operationId, message, generation);
    }
    startPendingIfAvailable();
}

void PrinterMediaPreparer::resetPreparationState() {
    const QString completedOperationId = operationId_;
    processDeadlineTimer_->stop();
    active_ = false;
    cancelling_ = false;
    preparationTimedOut_ = false;
    mediaPreparationDeadline_ = QDeadlineTimer();
    phase_ = PreparationPhase::Idle;
    operationId_.clear();
    devicePath_.clear();
    sourcePath_.clear();
    uploadPath_.clear();
    rawMediaPath_.clear();
    remoteName_.clear();
    stagedThumbnailTempPath_.clear();
    stagedThumbnailPath_.clear();
    stagedThumbnailSha256_.clear();
    preparedSha256_.clear();
    expectedSourceSha256_.clear();
    transform_ = tryxLegacyFitMediaTransform();
    recoveredVideo_ = false;
    productId_ = 0x1021;
    turrisMediaKind_ = 0;
    generation_ = 0;
    processOutput_.clear();
    if (!completedOperationId.isEmpty()) {
        QMutexLocker locker(&preparationCancellationMutex_);
        cancelledPreparationOperations_.remove(completedOperationId);
    }
}

void PrinterMediaPreparer::startPendingIfAvailable() {
    if (!hasPending_ || shuttingDown_) {
        hasPending_ = false;
        return;
    }
    const QString operationId = pendingOperationId_;
    const QString devicePath = pendingDevicePath_;
    const QString localPath = pendingLocalPath_;
    const QString expectedSourceSha256 =
        pendingExpectedSourceSha256_;
    const TryxRuntimeMediaTransform transform = pendingTransform_;
    const bool recoveredVideo = pendingRecoveredVideo_;
    const quint16 productId = pendingProductId_;
    const quint64 generation = pendingGeneration_;
    hasPending_ = false;
    pendingOperationId_.clear();
    pendingDevicePath_.clear();
    pendingLocalPath_.clear();
    pendingExpectedSourceSha256_.clear();
    pendingTransform_ = tryxLegacyFitMediaTransform();
    pendingRecoveredVideo_ = false;
    pendingProductId_ = 0x1021;
    pendingGeneration_ = 0;
    startPreparation(operationId, devicePath, localPath,
                     expectedSourceSha256, generation, transform,
                     recoveredVideo, productId);
}

void PrinterMediaPreparer::cancelStale(quint64 currentGeneration) {
    requestGenerationCancellation(currentGeneration);
    if (hasPending_ && pendingGeneration_ != currentGeneration) {
        hasPending_ = false;
        pendingOperationId_.clear();
        pendingDevicePath_.clear();
        pendingLocalPath_.clear();
        pendingExpectedSourceSha256_.clear();
        pendingTransform_ = tryxLegacyFitMediaTransform();
        pendingRecoveredVideo_ = false;
        pendingProductId_ = 0x1021;
        pendingGeneration_ = 0;
    }
    if (active_ && generation_ != currentGeneration) {
        cancelling_ = true;
        process_->kill();
    }
}

void PrinterMediaPreparer::cancelOperation(const QString &operationId) {
    requestOperationCancellation(operationId);
    bool operationRemovedBeforeStart = false;
    if (hasPending_ && pendingOperationId_ == operationId) {
        hasPending_ = false;
        pendingOperationId_.clear();
        pendingDevicePath_.clear();
        pendingLocalPath_.clear();
        pendingExpectedSourceSha256_.clear();
        pendingTransform_ = tryxLegacyFitMediaTransform();
        pendingRecoveredVideo_ = false;
        pendingProductId_ = 0x1021;
        pendingGeneration_ = 0;
        operationRemovedBeforeStart = true;
    }
    if (active_ && operationId_ == operationId) {
        cancelling_ = true;
        process_->kill();
        return;
    }
    if (operationRemovedBeforeStart || !active_ ||
        operationId_ != operationId) {
        QMutexLocker locker(&preparationCancellationMutex_);
        cancelledPreparationOperations_.remove(operationId);
    }
}

void PrinterMediaPreparer::validateRetryCache(
    const QString &validationId, const QString &preparedPath,
    const QString &expectedSha256) {
    const auto isCancelled = [this, validationId]() {
        QMutexLocker locker(&retryValidationMutex_);
        return cancelledRetryValidations_.contains(validationId);
    };
    if (isCancelled()) {
        emit retryCacheValidated(
            validationId, false, true,
            tr("Prepared-media validation was cancelled"));
        return;
    }
    const QFileInfo info(preparedPath);
    if (validationId.isEmpty() || !info.exists() || !info.isFile() ||
        info.isSymLink() ||
        info.size() <= 0 || !isSha256Hex(expectedSha256)) {
        emit retryCacheValidated(
            validationId, false, false,
            tr("Prepared media failed retry-cache validation"));
        return;
    }
    const QString actualSha256 = sha256File(preparedPath, isCancelled);
    if (isCancelled()) {
        emit retryCacheValidated(
            validationId, false, true,
            tr("Prepared-media validation was cancelled"));
        return;
    }
    if (actualSha256.isEmpty() || actualSha256 != expectedSha256) {
        emit retryCacheValidated(
            validationId, false, false,
            tr("Prepared media hash does not match the retry cache"));
        return;
    }
    emit retryCacheValidated(validationId, true, false, QString());
}

void PrinterMediaPreparer::releasePreparedFile(const QString &uploadPath) {
    deliveredPaths_.remove(uploadPath);
}

void PrinterMediaPreparer::shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    processDeadlineTimer_->stop();
    hasPending_ = false;
    pendingOperationId_.clear();
    pendingDevicePath_.clear();
    pendingLocalPath_.clear();
    pendingExpectedSourceSha256_.clear();
    pendingTransform_ = tryxLegacyFitMediaTransform();
    pendingRecoveredVideo_ = false;
    pendingProductId_ = 0x1021;
    pendingGeneration_ = 0;
    if (active_) {
        cancelling_ = true;
        process_->kill();
        process_->waitForFinished(3000);
        if (active_ && process_->state() == QProcess::NotRunning) {
            finishPreparation(process_->exitCode(),
                              process_->exitStatus() == QProcess::NormalExit);
        }
        if (active_) {
            QFile::remove(uploadPath_);
            QFile::remove(rawMediaPath_);
            QFile::remove(stagedThumbnailTempPath_);
            QFile::remove(stagedThumbnailPath_);
            resetPreparationState();
        }
    }
    for (const QString &path : std::as_const(deliveredPaths_)) {
        QFile::remove(path);
    }
    deliveredPaths_.clear();
}
