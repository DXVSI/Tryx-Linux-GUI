#include "pasemediaclient.h"
#include "printermediaupload.h"
#include "printermediahelpers_p.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSet>

#include <limits>
#include <utility>

using namespace tryx::printer_media;
using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;

PrinterProtocol::MediaPullResult PaseMediaClient::pullValidatedUserMedia(
    const QString &devicePath, const QString &mediaName, qint64 expectedSize,
    const MediaPullChunkSink &sink, const MediaPullProgress &progress,
    const OperationContext &context) {
    MediaPullResult result;
    result.mediaName = mediaName;
    result.fileSize = expectedSize;

    const auto cancel = [&result]() {
        result.cancelled = true;
        result.error = QObject::tr(
            "TRYX media pull was cancelled because the device state changed");
        return result;
    };
    const auto fail = [&result](const QString &error) {
        result.error = error;
        return result;
    };
    const auto rejectResponse = [this, &result](const QString &error) {
        result.error = error;
        channel_.closeDevice();
        return result;
    };

    if (operationIsCancelled(context)) {
        return cancel();
    }
    if (!sink) {
        return fail(QObject::tr("Media pull requires a bounded decoded-chunk sink"));
    }
    if (!isSafeUploadFileName(mediaName) || expectedSize <= 0 ||
        expectedSize > mediaPullMaximumBytes_) {
        return fail(
            QObject::tr("Selected media identity is not eligible for a bounded pull"));
    }

    const qint64 deadlineMs = mediaPullDeadlineOverrideMs_ > 0
                                  ? mediaPullDeadlineOverrideMs_
                                  : mediaPullDeadlineForSize(expectedSize);
    QElapsedTimer operationTimer;
    operationTimer.start();
    const auto deadlineExpired = [&operationTimer, deadlineMs]() {
        return operationTimer.elapsed() >= deadlineMs;
    };
    OperationContext boundedContext = context;
    boundedContext.isCancelled = [&context, &deadlineExpired]() {
        return operationIsCancelled(context) || deadlineExpired();
    };

    panorama::wire::v1::Request catalogRequest;
    catalogRequest.mutable_media_catalog_query();
    panorama::wire::v1::Response catalogResponse;
    QString transactionError;
    TransactionOutcome transactionOutcome = TransactionOutcome::NotSent;
    if (!channel_.execute(&catalogRequest, panorama::wire::v1::Response::kMediaCatalog,
                          &catalogResponse, devicePath, boundedContext,
                          &transactionError, &transactionOutcome)) {
        if (deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline during catalog preflight"));
        }
        if (transactionOutcome == TransactionOutcome::Cancelled ||
            operationIsCancelled(context)) {
            return cancel();
        }
        return fail(QObject::tr("Cannot read the fresh media catalog before pull: %1")
                        .arg(transactionError));
    }
    if (operationIsCancelled(context)) {
        return cancel();
    }
    if (deadlineExpired()) {
        return fail(QObject::tr(
            "Media pull exceeded its bounded operation deadline during catalog preflight"));
    }

    MediaPullCandidate candidate;
    QString candidateError;
    if (!resolveMediaPullCandidate(catalogResponse.media_catalog(), mediaName,
                                   expectedSize, mediaPullMaximumBytes_, &candidate,
                                   &candidateError)) {
        return fail(candidateError);
    }

    quint64 sessionId = 0;
    while (sessionId == 0) {
        sessionId = QRandomGenerator::global()->generate64();
    }

    QCryptographicHash rawHash(QCryptographicHash::Sha256);
    QCryptographicHash decodedHash(QCryptographicHash::Sha256);
    const quint64 exactFileSize = static_cast<quint64>(candidate.fileSize);
    quint64 offset = 0;
    int chunks = 0;
    while (offset < exactFileSize) {
        if (operationIsCancelled(context)) {
            return cancel();
        }
        if (deadlineExpired()) {
            return fail(
                QObject::tr("Media pull exceeded its bounded operation deadline"));
        }
        if (chunks >= mediaPullMaximumChunks_) {
            return fail(QObject::tr("Media pull exceeded the bounded chunk count"));
        }

        panorama::wire::v1::Request request;
        auto *read = request.mutable_media_read_chunk();
        read->set_remote_path(candidate.rawPath.constData(),
                              static_cast<size_t>(candidate.rawPath.size()));
        read->set_session_id(sessionId);
        read->set_offset(offset);

        panorama::wire::v1::Response response;
        transactionError.clear();
        transactionOutcome = TransactionOutcome::NotSent;
        if (!channel_.execute(&request, panorama::wire::v1::Response::kMediaReadChunk,
                              &response, devicePath, boundedContext, &transactionError,
                              &transactionOutcome, TransactionProfile::MediaPull)) {
            if (deadlineExpired()) {
                return fail(QObject::tr(
                    "Media pull exceeded its bounded operation deadline while waiting for a chunk"));
            }
            if (transactionOutcome == TransactionOutcome::Cancelled ||
                operationIsCancelled(context)) {
                return cancel();
            }
            return fail(QObject::tr("TRYX media pull request failed at offset %1: %2")
                            .arg(offset)
                            .arg(transactionError));
        }
        if (operationIsCancelled(context)) {
            return cancel();
        }
        if (deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline while receiving a chunk"));
        }

        const auto &readResponse = response.media_read_chunk();
        if (readResponse.status() != panorama::wire::v1::MediaReadChunkResponse::OK) {
            return fail(
                readResponse.status() ==
                        panorama::wire::v1::MediaReadChunkResponse::FILE_ERROR
                    ? QObject::tr(
                          "TRYX device reported a file error while pulling media")
                    : QObject::tr("TRYX device returned an unknown media pull status"));
        }

        const std::string &responsePath = readResponse.remote_path();
        const QByteArray echoedPath(responsePath.data(),
                                    static_cast<qsizetype>(responsePath.size()));
        if (echoedPath != candidate.rawPath) {
            return rejectResponse(QObject::tr(
                "TRYX media pull response changed the validated device path"));
        }
        if (readResponse.session_id() != sessionId) {
            return rejectResponse(
                QObject::tr("TRYX media pull response changed the session identifier"));
        }
        if (readResponse.offset() != offset) {
            return rejectResponse(QObject::tr(
                "TRYX media pull response offset does not match the requested offset"));
        }
        if (readResponse.file_size() != exactFileSize) {
            return rejectResponse(QObject::tr(
                "TRYX media pull response changed the fresh catalog file size"));
        }

        const std::string &responseData = readResponse.data();
        const QByteArray rawChunk(responseData.data(),
                                  static_cast<qsizetype>(responseData.size()));
        if (rawChunk.isEmpty()) {
            return rejectResponse(
                QObject::tr("TRYX media pull made no progress before end of file"));
        }
        const quint64 chunkSize = static_cast<quint64>(rawChunk.size());
        if (offset > std::numeric_limits<quint64>::max() - chunkSize ||
            chunkSize > exactFileSize - offset) {
            return rejectResponse(
                QObject::tr("TRYX media pull chunk exceeds the validated file size"));
        }

        bool decodeCancelled = false;
        const QByteArray decodedChunk =
            applyMediaPullXor(rawChunk, offset, &boundedContext, &decodeCancelled);
        if (decodeCancelled && deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline while decoding a chunk"));
        }
        if (decodeCancelled || operationIsCancelled(context)) {
            return cancel();
        }
        if (decodedChunk.size() != rawChunk.size()) {
            return rejectResponse(
                QObject::tr("TRYX media pull failed to decode a complete chunk"));
        }
        if (deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline while decoding a chunk"));
        }

        QString sinkError;
        if (!sink(static_cast<qint64>(offset), decodedChunk, &sinkError)) {
            return fail(sinkError.isEmpty()
                            ? QObject::tr("Decoded media pull sink rejected a chunk")
                            : sinkError);
        }
        if (operationIsCancelled(context)) {
            return cancel();
        }
        if (deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline while storing a chunk"));
        }

        rawHash.addData(rawChunk);
        decodedHash.addData(decodedChunk);
        offset += chunkSize;
        ++chunks;
        result.bytesDecoded = static_cast<qint64>(offset);
        result.chunkCount = chunks;
        if (progress) {
            progress(result.bytesDecoded, candidate.fileSize);
        }
        if (operationIsCancelled(context)) {
            return cancel();
        }
        if (deadlineExpired()) {
            return fail(QObject::tr(
                "Media pull exceeded its bounded operation deadline while reporting progress"));
        }
    }

    if (offset != exactFileSize) {
        return rejectResponse(
            QObject::tr("TRYX media pull did not finish at the validated file size"));
    }
    result.success = true;
    result.fileSize = candidate.fileSize;
    result.rawSha256 = QString::fromLatin1(rawHash.result().toHex());
    result.decodedSha256 = QString::fromLatin1(decodedHash.result().toHex());
    result.error.clear();
    return result;
}

#ifdef TRYX_PROTOCOL_TESTING
void PaseMediaClient::setMediaPullLimitsForTesting(qint64 maximumBytes,
                                                   int maximumChunks, int deadlineMs) {
    mediaPullMaximumBytes_ = qMax<qint64>(1, maximumBytes);
    mediaPullMaximumChunks_ = qMax(1, maximumChunks);
    mediaPullDeadlineOverrideMs_ = qMax(1, deadlineMs);
}
#endif

PrinterProtocol::MediaListResult
PaseMediaClient::readMediaList(const QString &devicePath,
                               const OperationContext &context) {
    if (!productProfile_.mediaCatalogSupported) {
        return {false,
                unsupportedCapabilityError(productProfile_,
                                           QStringLiteral("media catalog operations")),
                {}};
    }
    panorama::wire::v1::Request request;
    request.mutable_media_catalog_query();
    panorama::wire::v1::Response response;
    QString error;
    if (!channel_.execute(&request, panorama::wire::v1::Response::kMediaCatalog,
                          &response, devicePath, context, &error)) {
        return {false, error, {}};
    }

    QList<MediaFile> files;
    const auto appendFiles = [&files](const auto &protobufList, MediaSource source) {
        for (const auto &media : protobufList) {
            const QString name = normalizedMediaName(media);
            if (isSafeDeviceMediaName(name)) {
                files.append({name, media.file_size(), media.read_only(), source});
            }
        }
    };
    appendFiles(response.media_catalog().media_file_list(), MediaSource::User);
    appendFiles(response.media_catalog().preset_file_list(), MediaSource::Preset);
    return {true, {}, files};
}

PrinterProtocol::MediaPullResult
PaseMediaClient::pullUserMedia(const QString &devicePath, const QString &mediaName,
                               qint64 expectedSize, const MediaPullChunkSink &sink,
                               const MediaPullProgress &progress,
                               const OperationContext &context) {
    if (!productProfile_.mediaCatalogSupported) {
        MediaPullResult result;
        result.mediaName = mediaName;
        result.fileSize = expectedSize;
        result.error = unsupportedCapabilityError(
            productProfile_, QStringLiteral("media pull operations"));
        return result;
    }
    if (!isSafeUploadFileNameForProfile(mediaName, productProfile_)) {
        MediaPullResult result;
        result.mediaName = mediaName;
        result.fileSize = expectedSize;
        result.error =
            QObject::tr("Selected media geometry is not supported by this device");
        return result;
    }
    return pullValidatedUserMedia(devicePath, mediaName, expectedSize, sink, progress,
                                  context);
}

PrinterProtocol::MediaReferenceResult PaseMediaClient::readUserMediaReferences(
    const QString &devicePath, const QString &mediaName, qint64 expectedSize,
    const QString &expectedReplacementName, qint64 expectedReplacementSize,
    const OperationContext &context) {
    MediaReferenceResult result;
    if (!productProfile_.mediaCatalogSupported) {
        result.error = unsupportedCapabilityError(
            productProfile_, QStringLiteral("media catalog operations"));
        return result;
    }
    if (!isSafeUploadFileName(mediaName) || expectedSize <= 0 ||
        expectedSize > static_cast<qint64>(std::numeric_limits<quint32>::max())) {
        result.error = QObject::tr(
            "Selected media identity is not eligible for reference preflight");
        return result;
    }
    const bool replacementExpected =
        !expectedReplacementName.isEmpty() || expectedReplacementSize != 0;
    if (replacementExpected &&
        (!isSafeUploadFileName(expectedReplacementName) ||
         expectedReplacementName == mediaName || expectedReplacementSize <= 0 ||
         expectedReplacementSize >
             static_cast<qint64>(std::numeric_limits<quint32>::max()))) {
        result.error = QObject::tr(
            "Expected replacement media identity is not eligible for reference preflight");
        return result;
    }

    const MediaListResult currentList = readMediaList(devicePath, context);
    if (!currentList.success) {
        result.error =
            QObject::tr(
                "Cannot read the fresh media catalog before reference preflight: %1")
                .arg(currentList.error);
        return result;
    }

    QList<MediaFile> matches;
    QList<MediaFile> replacementMatches;
    for (const MediaFile &media : currentList.files) {
        if (media.name == mediaName) {
            matches.append(media);
        }
        if (replacementExpected && media.name == expectedReplacementName) {
            replacementMatches.append(media);
        }
    }
    result.originalIdentityVerified =
        matches.size() == 1 && matches.constFirst().source == MediaSource::User &&
        !matches.constFirst().readOnly &&
        matches.constFirst().size == static_cast<quint32>(expectedSize);
    result.replacementIdentityVerified =
        replacementExpected && replacementMatches.size() == 1 &&
        replacementMatches.constFirst().source == MediaSource::User &&
        !replacementMatches.constFirst().readOnly &&
        replacementMatches.constFirst().size ==
            static_cast<quint32>(expectedReplacementSize);
    if (!result.originalIdentityVerified) {
        result.error =
            matches.isEmpty()
                ? QObject::tr(
                      "Selected media is no longer present in the fresh device catalog")
                : QObject::tr(
                      "Selected media identity changed in the fresh device catalog");
        return result;
    }
    result.media = matches.constFirst();
    if (replacementExpected && !result.replacementIdentityVerified) {
        result.error =
            replacementMatches.isEmpty()
                ? QObject::tr(
                      "The expected replacement copy is absent from the fresh device catalog")
                : QObject::tr(
                      "The expected replacement identity changed in the fresh device catalog");
        return result;
    }

    panorama::wire::v1::Request configRequest;
    configRequest.mutable_user_configuration_query();
    panorama::wire::v1::Response configResponse;
    QString configError;
    if (!channel_.execute(&configRequest,
                          panorama::wire::v1::Response::kUserConfiguration,
                          &configResponse, devicePath, context, &configError)) {
        result.error =
            QObject::tr(
                "Cannot read device configuration during reference preflight: %1")
                .arg(configError);
        return result;
    }

    const auto &configuration = configResponse.user_configuration();
    if (!configuration.has_work_config() ||
        !decodePaseActiveLayout(configuration.work_config(), &result.activeScreenMode,
                                &result.activePlayMode, &result.activeMedia,
                                &result.error)) {
        if (result.error.isEmpty()) {
            result.error =
                QObject::tr("TRYX user configuration is missing work configuration");
        }
        return result;
    }
    const auto &work = configuration.work_config();
    const auto &filter = configuration.filter_config();
    result.references = {
        normalizedMediaReference(configuration.poweron_config().media_file()),
        normalizedMediaReference(configuration.standby_config().media_file()),
        normalizedMediaReference(work.single_mode_media_file()),
        normalizedMediaReference(work.dual_mode_left_media_file()),
        normalizedMediaReference(work.dual_mode_right_media_file()),
        normalizedMediaReference(work.kaleidoscope_media_file()),
        normalizedMediaReference(filter.filter_file()),
        normalizedMediaReference(filter.dual_mode_left_file()),
        normalizedMediaReference(filter.dual_mode_right_file())};
    const QStringList referenceSlotNames = {
        QStringLiteral("PowerOn"),        QStringLiteral("Standby"),
        QStringLiteral("Single"),         QStringLiteral("DualLeft"),
        QStringLiteral("DualRight"),      QStringLiteral("Kaleidoscope"),
        QStringLiteral("FilterSingle"),   QStringLiteral("FilterDualLeft"),
        QStringLiteral("FilterDualRight")};
    for (qsizetype index = 0; index < result.references.size(); ++index) {
        if (result.references.at(index) == mediaName) {
            result.referencingSlots.append(referenceSlotNames.at(index));
        }
    }
    result.success = true;
    return result;
}

PrinterProtocol::DeleteResult PaseMediaClient::removeUserMedia(
    const QString &devicePath, const QStringList &fileNames,
    const BeforeDeleteDispatch &beforeDispatch, const DeleteProgress &progress,
    const OperationContext &context, bool reconcileOnly, qint64 expectedSingleSize,
    const QString &expectedReplacementName, qint64 expectedReplacementSize) {
    DeleteResult result;
    if (!productProfile_.mediaCatalogSupported) {
        result.outcome = MutationOutcome::Rejected;
        result.error = unsupportedCapabilityError(productProfile_,
                                                  QStringLiteral("media deletion"));
        return result;
    }
    if (fileNames.isEmpty()) {
        result.error = QObject::tr("No media files were selected for deletion");
        return result;
    }
    if (expectedSingleSize < 0 ||
        expectedSingleSize > static_cast<qint64>(std::numeric_limits<quint32>::max()) ||
        (expectedSingleSize > 0 && fileNames.size() != 1)) {
        result.error = QObject::tr("Expected delete media identity is invalid");
        return result;
    }
    const bool replacementExpected =
        !expectedReplacementName.isEmpty() || expectedReplacementSize != 0;
    if (replacementExpected &&
        (fileNames.size() != 1 || expectedSingleSize <= 0 ||
         !isSafeUploadFileName(expectedReplacementName) ||
         expectedReplacementName == fileNames.constFirst() ||
         expectedReplacementSize <= 0 ||
         expectedReplacementSize >
             static_cast<qint64>(std::numeric_limits<quint32>::max()))) {
        result.error =
            QObject::tr("Expected replacement identity before deletion is invalid");
        return result;
    }
    QSet<QString> uniqueNames;
    for (const QString &fileName : fileNames) {
        if (!isSafeUploadFileName(fileName) ||
            fileName.startsWith(QStringLiteral("default_"), Qt::CaseInsensitive) ||
            uniqueNames.contains(fileName)) {
            result.error = QObject::tr("Media file is not eligible for deletion: %1")
                               .arg(fileName);
            return result;
        }
        uniqueNames.insert(fileName);
    }

    MediaListResult currentList = readMediaList(devicePath, context);
    if (!currentList.success) {
        result.outcome = reconcileOnly ? MutationOutcome::PartialOrUnknown
                                       : MutationOutcome::NotStarted;
        result.error = QObject::tr("Cannot read the media list before deletion: %1")
                           .arg(currentList.error);
        return result;
    }
    result.files = currentList.files;

    if (reconcileOnly) {
        const QString target = fileNames.constFirst();
        result.currentName = target;
        QList<MediaFile> matches;
        for (const MediaFile &media : std::as_const(currentList.files)) {
            if (media.name == target) {
                matches.append(media);
            }
        }
        if (matches.isEmpty()) {
            result.success = true;
            result.outcome = MutationOutcome::Succeeded;
            result.deletedNames.append(target);
        } else if (matches.size() == 1 &&
                   matches.constFirst().source == MediaSource::User &&
                   !matches.constFirst().readOnly &&
                   (expectedSingleSize == 0 ||
                    matches.constFirst().size ==
                        static_cast<quint32>(expectedSingleSize))) {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error =
                QObject::tr(
                    "The file is still present during delete reconciliation; FileRemove will not be repeated: %1")
                    .arg(target);
        } else {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error =
                QObject::tr(
                    "The media identity changed during delete reconciliation; FileRemove will not be repeated: %1")
                    .arg(target);
        }
        return result;
    }

    constexpr int kMaxDeleteReconciliationReads = 4;

    for (int index = 0; index < fileNames.size(); ++index) {
        const QString target = fileNames.at(index);
        result.currentName = target;
        if (progress) {
            progress(QStringLiteral("DeletePreflight"), target, index,
                     fileNames.size());
        }

        if (index > 0) {
            currentList = readMediaList(devicePath, context);
            if (!currentList.success) {
                result.outcome = MutationOutcome::NotStarted;
                result.error =
                    QObject::tr("Cannot refresh the media list before deleting %1: %2")
                        .arg(target, currentList.error);
                return result;
            }
            result.files = currentList.files;
        }
        QList<MediaFile> matches;
        for (const MediaFile &media : std::as_const(currentList.files)) {
            if (media.name == target) {
                matches.append(media);
            }
        }
        if (matches.size() != 1 || matches.constFirst().source != MediaSource::User ||
            matches.constFirst().readOnly ||
            (expectedSingleSize > 0 &&
             matches.constFirst().size != static_cast<quint32>(expectedSingleSize))) {
            result.outcome = MutationOutcome::NotStarted;
            result.error =
                matches.isEmpty()
                    ? QObject::tr("Media file is absent from the fresh device list: %1")
                          .arg(target)
                : expectedSingleSize > 0
                    ? QObject::tr("Media identity changed before deletion: %1")
                          .arg(target)
                    : QObject::tr("Media file is protected or ambiguous: %1")
                          .arg(target);
            return result;
        }

        panorama::wire::v1::Request configRequest;
        configRequest.mutable_user_configuration_query();
        panorama::wire::v1::Response configResponse;
        QString configError;
        if (!channel_.execute(&configRequest,
                              panorama::wire::v1::Response::kUserConfiguration,
                              &configResponse, devicePath, context, &configError)) {
            result.outcome = MutationOutcome::NotStarted;
            result.error =
                QObject::tr("Cannot verify device configuration before deleting %1: %2")
                    .arg(target, configError);
            return result;
        }
        const panorama::wire::v1::UserConfiguration &config =
            configResponse.user_configuration();
        QStringList references;
        if (config.has_poweron_config()) {
            references.append(
                normalizedMediaReference(config.poweron_config().media_file()));
        }
        if (config.has_standby_config()) {
            references.append(
                normalizedMediaReference(config.standby_config().media_file()));
        }
        if (config.has_work_config()) {
            const auto &work = config.work_config();
            references.append(normalizedMediaReference(work.single_mode_media_file()));
            references.append(
                normalizedMediaReference(work.dual_mode_left_media_file()));
            references.append(
                normalizedMediaReference(work.dual_mode_right_media_file()));
            references.append(normalizedMediaReference(work.kaleidoscope_media_file()));
        }
        if (config.has_filter_config()) {
            const auto &filter = config.filter_config();
            references.append(normalizedMediaReference(filter.filter_file()));
            references.append(normalizedMediaReference(filter.dual_mode_left_file()));
            references.append(normalizedMediaReference(filter.dual_mode_right_file()));
        }
        references.removeAll(QString());
        if (references.contains(target)) {
            result.outcome = MutationOutcome::NotStarted;
            result.error =
                QObject::tr(
                    "Media file is referenced by the active device configuration: %1")
                    .arg(target);
            return result;
        }

        if (expectedSingleSize > 0) {
            const MediaListResult dispatchList = readMediaList(devicePath, context);
            if (!dispatchList.success) {
                result.outcome = MutationOutcome::NotStarted;
                result.error =
                    QObject::tr(
                        "Cannot revalidate media identity immediately before deleting %1: %2")
                        .arg(target, dispatchList.error);
                return result;
            }
            result.files = dispatchList.files;
            QList<MediaFile> dispatchMatches;
            for (const MediaFile &media : dispatchList.files) {
                if (media.name == target) {
                    dispatchMatches.append(media);
                }
            }
            if (dispatchMatches.size() != 1 ||
                dispatchMatches.constFirst().source != MediaSource::User ||
                dispatchMatches.constFirst().readOnly ||
                dispatchMatches.constFirst().size !=
                    static_cast<quint32>(expectedSingleSize)) {
                result.outcome = MutationOutcome::NotStarted;
                result.error =
                    QObject::tr(
                        "Media identity changed immediately before deletion: %1")
                        .arg(target);
                return result;
            }
            if (replacementExpected) {
                QList<MediaFile> replacementMatches;
                for (const MediaFile &media : dispatchList.files) {
                    if (media.name == expectedReplacementName) {
                        replacementMatches.append(media);
                    }
                }
                if (replacementMatches.size() != 1 ||
                    replacementMatches.constFirst().source != MediaSource::User ||
                    replacementMatches.constFirst().readOnly ||
                    replacementMatches.constFirst().size !=
                        static_cast<quint32>(expectedReplacementSize)) {
                    result.outcome = MutationOutcome::NotStarted;
                    result.error =
                        QObject::tr(
                            "Replacement identity changed immediately before deleting the original: %1")
                            .arg(expectedReplacementName);
                    return result;
                }
            }
            matches = dispatchMatches;
        }

        QString dispatchError;
        if (!beforeDispatch ||
            !beforeDispatch(index, matches.constFirst(), &dispatchError)) {
            result.outcome = operationIsCancelled(context)
                                 ? MutationOutcome::Cancelled
                                 : MutationOutcome::NotStarted;
            result.error = dispatchError.isEmpty()
                               ? QObject::tr("Deletion was stopped before dispatch")
                               : dispatchError;
            return result;
        }

        if (progress) {
            progress(QStringLiteral("Deleting"), target, index, fileNames.size());
        }
        panorama::wire::v1::Request removeRequest;
        auto *remove = removeRequest.mutable_file_removal();
        remove->set_file_name(target.toStdString());
        remove->set_file_type("media");
        panorama::wire::v1::Response removeResponse;
        QString removeError;
        PrinterTransactionChannel::TransactionOutcome transactionOutcome =
            PrinterTransactionChannel::TransactionOutcome::NotSent;
        const bool acknowledged = channel_.execute(
            &removeRequest, panorama::wire::v1::Response::kAcknowledgement,
            &removeResponse, devicePath, context, &removeError, &transactionOutcome,
            PrinterTransactionChannel::TransactionProfile::Default, true, true);
        result.commandAcknowledged = result.commandAcknowledged || acknowledged;
        const bool mayHaveStarted =
            transactionOutcome !=
                PrinterTransactionChannel::TransactionOutcome::NotSent &&
            transactionOutcome !=
                PrinterTransactionChannel::TransactionOutcome::Cancelled;
        if (!mayHaveStarted) {
            result.outcome =
                transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::Cancelled
                    ? MutationOutcome::Cancelled
                    : MutationOutcome::NotStarted;
            result.error = removeError.isEmpty()
                               ? QObject::tr("FileRemove was not sent")
                               : removeError;
            return result;
        }
        const bool connectionCanReconcile =
            acknowledged ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::AcknowledgementTimeout ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::Rejected;
        if (!connectionCanReconcile) {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error =
                removeError.isEmpty()
                    ? QObject::tr(
                          "FileRemove may have been sent, but the transport cannot safely reconcile FileList")
                    : removeError;
            return result;
        }

        if (progress) {
            progress(QStringLiteral("ReconcilingDelete"), target, index,
                     fileNames.size());
        }
        bool reconciliationReadSucceeded = false;
        bool targetStillPresent = true;
        QString reconciliationError;
        for (int attempt = 0; attempt < kMaxDeleteReconciliationReads; ++attempt) {
            currentList = readMediaList(devicePath, context);
            if (!currentList.success) {
                reconciliationError = currentList.error;
                continue;
            }
            reconciliationReadSucceeded = true;
            result.files = currentList.files;
            targetStillPresent = std::any_of(
                currentList.files.cbegin(), currentList.files.cend(),
                [&target](const MediaFile &media) { return media.name == target; });
            if (!targetStillPresent) {
                break;
            }
        }
        if (!reconciliationReadSucceeded) {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error =
                QObject::tr(
                    "FileRemove may have been sent, but FileList reconciliation failed for %1: %2")
                    .arg(target, reconciliationError.isEmpty() ? removeError
                                                               : reconciliationError);
            return result;
        }
        if (targetStillPresent) {
            result.outcome =
                transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::Rejected
                    ? MutationOutcome::Rejected
                    : MutationOutcome::PartialOrUnknown;
            result.error =
                transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::Rejected
                    ? QObject::tr("The device rejected deletion of %1").arg(target)
                    : QObject::tr(
                          "The device still reports %1 after bounded reconciliation; FileRemove will not be repeated")
                          .arg(target);
            return result;
        }
        result.deletedNames.append(target);
        if (progress) {
            progress(QStringLiteral("ReconcilingDelete"), target, index + 1,
                     fileNames.size());
        }
    }

    result.success = true;
    result.outcome = MutationOutcome::Succeeded;
    return result;
}

bool PaseMediaClient::uploadMedia(const QString &devicePath, const QString &localPath,
                                  const QString &remoteFileName, QString *uploadedName,
                                  QString *errorMessage, const UploadProgress &progress,
                                  const OperationContext &context,
                                  MutationDetails *mutationDetails,
                                  const QString &expectedSha256) {
    if (!validatePrinterUploadRequest(productProfile_, remoteFileName, errorMessage,
                                      mutationDetails)) {
        return false;
    }
    PrinterMediaUploadOptions options;
    options.allowKeepalive = productProfile_.idleMode != PrinterIdleMode::TransferOnly;
    return uploadPrinterMedia(channel_, options, devicePath, localPath, remoteFileName,
                              uploadedName, errorMessage, progress, context,
                              mutationDetails, expectedSha256);
}
