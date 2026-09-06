#include "printermediahelpers_p.h"
#include "printermediaidentity.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QFileInfo>

namespace tryx::printer_media {

using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;

bool validatePrinterUploadRequest(const PrinterProductProfile &productProfile,
                                  const QString &remoteFileName, QString *errorMessage,
                                  PrinterProtocol::MutationDetails *mutationDetails) {
    using MutationOutcome = PrinterProtocol::MutationOutcome;
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("Validating");
    }
    if (!productProfile.mediaUploadSupported) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(productProfile,
                                                       QStringLiteral("media upload"));
        }
        return false;
    }
    if (!isSafeUploadFileNameForProfile(remoteFileName, productProfile)) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr("Media file name is not supported for %1: %2")
                                .arg(printerProductIdString(productProfile.productId),
                                     remoteFileName);
        }
        return false;
    }
    return true;
}

QString unsupportedCapabilityError(const PrinterProductProfile &productProfile,
                                   const QString &capability) {
    return QObject::tr("TRYX %1 does not support %2")
        .arg(printerProductIdString(productProfile.productId), capability);
}

QString normalizedMediaName(const panorama::wire::v1::MediaEntry &media) {
    QString name = QString::fromStdString(media.file_path()).trimmed();
    if (name.isEmpty()) {
        return {};
    }

    const qsizetype userdataIndex = name.indexOf(QStringLiteral("/userdata/"));
    const qsizetype pcMediaIndex = name.indexOf(QStringLiteral("/sdcard/pcMedia/"));
    if (userdataIndex >= 0 && (pcMediaIndex < 0 || userdataIndex < pcMediaIndex)) {
        name = name.mid(userdataIndex);
    } else if (pcMediaIndex >= 0) {
        name = name.mid(pcMediaIndex);
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        name = QFileInfo(name).fileName();
    }

    const QString extension = QString::fromStdString(media.file_ext()).trimmed();
    if (!extension.isEmpty() && !name.endsWith(extension, Qt::CaseInsensitive)) {
        name += extension.startsWith(QLatin1Char('.')) ? extension
                                                       : QLatin1Char('.') + extension;
    }
    return name;
}

QString normalizedMediaReference(const std::string &value) {
    QString reference = QString::fromStdString(value).trimmed();
    reference.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const qsizetype separator = reference.lastIndexOf(QLatin1Char('/'));
    if (separator >= 0) {
        reference = reference.mid(separator + 1);
    }
    return reference.trimmed();
}

bool decodePaseActiveLayout(const panorama::wire::v1::WorkConfiguration &work,
                            QString *screenMode, QString *playMode, QStringList *media,
                            QString *errorMessage) {
    if (!screenMode || !playMode || !media) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX user configuration has no active-layout output");
        }
        return false;
    }

    QString decodedScreenMode;
    QString decodedPlayMode;
    QStringList decodedMedia;
    switch (work.media_mode()) {
    case panorama::wire::v1::WorkConfiguration::MEDIA_DUAL:
        decodedScreenMode = QStringLiteral("Screen Splitting");
        decodedPlayMode = QStringLiteral("Single");
        decodedMedia = {normalizedMediaReference(work.dual_mode_left_media_file()),
                        normalizedMediaReference(work.dual_mode_right_media_file())};
        break;
    case panorama::wire::v1::WorkConfiguration::MEDIA_KALEIDOSCOPE:
        decodedScreenMode = QStringLiteral("Kaleidoscope");
        decodedPlayMode = QStringLiteral("Single");
        decodedMedia = {normalizedMediaReference(work.kaleidoscope_media_file())};
        break;
    case panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE:
        decodedScreenMode = QStringLiteral("Full Screen");
        switch (work.loop_mode()) {
        case panorama::wire::v1::WorkConfiguration::LOOP_ALL:
            decodedPlayMode = QStringLiteral("Loop");
            break;
        case panorama::wire::v1::WorkConfiguration::LOOP_RANDOM:
            decodedPlayMode = QStringLiteral("Shuffle");
            break;
        case panorama::wire::v1::WorkConfiguration::LOOP_SINGLE:
            decodedPlayMode = QStringLiteral("Single");
            break;
        default:
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX user configuration has an unknown playback mode");
            }
            return false;
        }
        decodedMedia = {normalizedMediaReference(work.single_mode_media_file())};
        break;
    default:
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX user configuration has an unknown screen mode");
        }
        return false;
    }

    *screenMode = decodedScreenMode;
    *playMode = decodedPlayMode;
    *media = decodedMedia;
    return true;
}

bool isSafeDeviceMediaName(const QString &fileName) {
    return tryx::printer_media_identity::isSafePrinterDeviceMediaName(fileName);
}

bool isSafeUploadFileName(const QString &fileName) {
    return tryx::printer_media_identity::isSafePrinterUploadMediaName(fileName);
}

bool isSafeUploadFileNameForProfile(const QString &fileName,
                                    const PrinterProductProfile &productProfile) {
    return tryx::printer_media_identity::printerMediaNameMatchesProfile(fileName,
                                                                        productProfile);
}

bool validateMediaPullPath(const QByteArray &rawPath, const QString &mediaName) {
    static const QByteArray prefix = QByteArrayLiteral("/userdata/user/");
    if (!isSafeUploadFileName(mediaName) || rawPath.isEmpty() ||
        rawPath.size() > prefix.size() + 512 || !rawPath.startsWith(prefix) ||
        rawPath != prefix + mediaName.toUtf8() ||
        QString::fromUtf8(rawPath).toUtf8() != rawPath) {
        return false;
    }

    for (const char byte : rawPath) {
        const auto value = static_cast<unsigned char>(byte);
        if (value == 0 || value < 0x20 || value == 0x7f || byte == '\\') {
            return false;
        }
    }
    return true;
}

QByteArray applyMediaPullXor(const QByteArray &bytes, quint64 absoluteOffset,
                             const PrinterProtocol::OperationContext *context,
                             bool *cancelled) {
    if (cancelled) {
        *cancelled = false;
    }
    QByteArray transformed = bytes;
    for (qsizetype index = 0; index < transformed.size(); ++index) {
        if (context && index % kMediaPullCancellationCheckInterval == 0 &&
            operationIsCancelled(*context)) {
            if (cancelled) {
                *cancelled = true;
            }
            return {};
        }
        const quint8 mask =
            static_cast<quint8>((absoluteOffset + static_cast<quint64>(index)) & 0xffU);
        transformed[index] =
            static_cast<char>(static_cast<quint8>(transformed.at(index)) ^ mask);
    }
    return transformed;
}

qint64 mediaPullDeadlineForSize(qint64 fileSize) {
    const qint64 roundedMiB =
        qMax<qint64>(1, (fileSize + kBytesPerMiB - 1) / kBytesPerMiB);
    return qMin(kMediaPullDeadlineHardLimitMs,
                kMediaPullDeadlineBaseMs + roundedMiB * kMediaPullDeadlinePerMiBMs);
}

bool resolveMediaPullCandidate(const panorama::wire::v1::MediaCatalog &catalog,
                               const QString &mediaName, qint64 expectedSize,
                               qint64 maximumBytes, MediaPullCandidate *candidate,
                               QString *errorMessage) {
    if (!candidate) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Media pull candidate storage is not available");
        }
        return false;
    }

    int matchingEntries = 0;
    bool matchedPreset = false;
    panorama::wire::v1::MediaEntry selected;
    const auto inspect = [&](const auto &entries, bool preset) {
        for (const auto &entry : entries) {
            if (normalizedMediaName(entry) != mediaName) {
                continue;
            }
            ++matchingEntries;
            if (preset) {
                matchedPreset = true;
            } else {
                selected = entry;
            }
        }
    };
    inspect(catalog.media_file_list(), false);
    inspect(catalog.preset_file_list(), true);

    if (matchingEntries != 1 || matchedPreset) {
        if (errorMessage) {
            *errorMessage =
                matchingEntries == 0
                    ? QObject::tr(
                          "Selected media is no longer present in the fresh device catalog")
                    : QObject::tr(
                          "Selected media is not a unique writable user entry in the fresh device catalog");
        }
        return false;
    }
    if (selected.read_only()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Selected media is read-only and cannot be pulled");
        }
        return false;
    }
    const qint64 catalogSize = static_cast<qint64>(selected.file_size());
    if (catalogSize <= 0 || catalogSize != expectedSize || catalogSize > maximumBytes) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Selected media size changed or exceeds the bounded pull limit");
        }
        return false;
    }

    const std::string &path = selected.file_path();
    const QByteArray rawPath(path.data(), static_cast<qsizetype>(path.size()));
    if (!validateMediaPullPath(rawPath, mediaName)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Selected media has an unsafe or ambiguous device path");
        }
        return false;
    }

    candidate->rawPath = rawPath;
    candidate->mediaName = mediaName;
    candidate->fileSize = catalogSize;
    return true;
}

QString transmitStatusText(panorama::wire::v1::TransferStatus::Code status) {
    switch (status) {
    case panorama::wire::v1::TransferStatus::OK:
        return QObject::tr("OK");
    case panorama::wire::v1::TransferStatus::SPACE_NOT_ENOUGH:
        return QObject::tr("not enough device storage");
    case panorama::wire::v1::TransferStatus::FILE_ERROR:
        return QObject::tr("file error");
    case panorama::wire::v1::TransferStatus::CHECKSUM_FAILURE:
        return QObject::tr("CRC check failed");
    default:
        break;
    }
    return QObject::tr("unknown transfer status");
}

} // namespace tryx::printer_media
