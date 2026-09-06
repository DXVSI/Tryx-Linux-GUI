#include "turrismediaformat.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QSaveFile>
#include <QStringList>

#include <array>
#include <limits>

namespace {

constexpr qint64 kMaximumBlobBytes = 500LL * 1024LL * 1024LL;
constexpr quint32 kMediaMagic = 0x4D584844U;
constexpr quint32 kHeaderVersion = 1U;
constexpr quint32 kFramesPerSecond = 30U;
constexpr qsizetype kMaximumMetadataBytes = 4096;
constexpr char kMediaDescription[] =
    "Tryx media header v1, fps=30, size=1280x720";

void appendProtoVarint(QByteArray *output, quint64 value) {
    while (value >= 0x80U) {
        output->append(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }
    output->append(static_cast<char>(value));
}

void appendProtoVarintField(QByteArray *output, quint32 fieldNumber,
                            quint64 value) {
    appendProtoVarint(output, static_cast<quint64>(fieldNumber) << 3U);
    appendProtoVarint(output, value);
}

void appendProtoStringField(QByteArray *output, quint32 fieldNumber,
                            const QByteArray &value) {
    appendProtoVarint(output,
                      (static_cast<quint64>(fieldNumber) << 3U) | 2U);
    appendProtoVarint(output, static_cast<quint64>(value.size()));
    output->append(value);
}

QByteArray mediaMetadata(quint32 kind, quint64 frameCount) {
    QByteArray metadata;
    appendProtoVarintField(&metadata, 1U, kMediaMagic);
    appendProtoStringField(&metadata, 2U, QByteArray(kMediaDescription));
    appendProtoVarintField(&metadata, 3U, kind);
    appendProtoVarintField(&metadata, 4U, kHeaderVersion);
    appendProtoVarintField(&metadata, 5U, kFramesPerSecond);
    appendProtoVarintField(&metadata, 6U, tryx::turris_media::kWidth);
    appendProtoVarintField(&metadata, 7U, tryx::turris_media::kHeight);
    appendProtoVarintField(&metadata, 8U, frameCount);
    return metadata;
}

bool writeAll(QIODevice *device, const QByteArray &bytes) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = device->write(
            bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

struct MediaMetadata {
    std::array<bool, 9> seen{};
    quint64 magic = 0;
    QByteArray description;
    quint64 kind = 0;
    quint64 headerVersion = 0;
    quint64 framesPerSecond = 0;
    quint64 width = 0;
    quint64 height = 0;
    quint64 frameCount = 0;
};

bool takeCanonicalProtoVarint(const QByteArray &bytes, qsizetype *offset,
                              quint64 *value) {
    if (!offset || !value || *offset < 0 || *offset >= bytes.size()) {
        return false;
    }

    quint64 decoded = 0;
    for (int index = 0; index < 10; ++index) {
        if (*offset >= bytes.size()) {
            return false;
        }
        const quint8 byte = static_cast<quint8>(bytes.at((*offset)++));
        const quint8 payload = byte & 0x7fU;
        if (index == 9 && payload > 1U) {
            return false;
        }
        decoded |= static_cast<quint64>(payload) << (index * 7);
        if ((byte & 0x80U) == 0) {
            int canonicalSize = 1;
            for (quint64 remaining = decoded; remaining >= 0x80U;
                 remaining >>= 7U) {
                ++canonicalSize;
            }
            if (canonicalSize != index + 1) {
                return false;
            }
            *value = decoded;
            return true;
        }
    }
    return false;
}

bool parseMediaMetadata(const QByteArray &bytes,
                        MediaMetadata *metadata,
                        QString *errorMessage) {
    const auto reject = [errorMessage](const QString &reason) {
        if (errorMessage) {
            *errorMessage = reason;
        }
        return false;
    };
    if (!metadata || bytes.isEmpty() ||
        bytes.size() > kMaximumMetadataBytes) {
        return reject(QStringLiteral("metadata size is invalid"));
    }

    qsizetype offset = 0;
    while (offset < bytes.size()) {
        quint64 key = 0;
        if (!takeCanonicalProtoVarint(bytes, &offset, &key) || key == 0) {
            return reject(QStringLiteral("metadata field key is invalid"));
        }
        const quint64 fieldNumber = key >> 3U;
        const quint64 wireType = key & 0x07U;
        if (fieldNumber < 1 || fieldNumber > 8 ||
            metadata->seen.at(static_cast<size_t>(fieldNumber))) {
            return reject(QStringLiteral(
                "metadata contains an unknown or duplicate field"));
        }

        if (fieldNumber == 2) {
            if (wireType != 2) {
                return reject(QStringLiteral(
                    "metadata description has the wrong wire type"));
            }
            quint64 length = 0;
            if (!takeCanonicalProtoVarint(bytes, &offset, &length) ||
                length != sizeof(kMediaDescription) - 1 ||
                length > static_cast<quint64>(bytes.size() - offset)) {
                return reject(QStringLiteral(
                    "metadata description length is invalid"));
            }
            metadata->description = bytes.mid(
                offset, static_cast<qsizetype>(length));
            offset += static_cast<qsizetype>(length);
        } else {
            if (wireType != 0) {
                return reject(QStringLiteral(
                    "metadata numeric field has the wrong wire type"));
            }
            quint64 fieldValue = 0;
            if (!takeCanonicalProtoVarint(bytes, &offset, &fieldValue)) {
                return reject(QStringLiteral(
                    "metadata numeric field is invalid"));
            }
            switch (fieldNumber) {
            case 1:
                metadata->magic = fieldValue;
                break;
            case 3:
                metadata->kind = fieldValue;
                break;
            case 4:
                metadata->headerVersion = fieldValue;
                break;
            case 5:
                metadata->framesPerSecond = fieldValue;
                break;
            case 6:
                metadata->width = fieldValue;
                break;
            case 7:
                metadata->height = fieldValue;
                break;
            case 8:
                metadata->frameCount = fieldValue;
                break;
            default:
                return reject(QStringLiteral("metadata field is invalid"));
            }
        }
        metadata->seen.at(static_cast<size_t>(fieldNumber)) = true;
    }

    for (size_t fieldNumber = 1; fieldNumber < metadata->seen.size();
         ++fieldNumber) {
        if (!metadata->seen.at(fieldNumber)) {
            return reject(QStringLiteral("metadata is missing a field"));
        }
    }
    return true;
}

}  // namespace

namespace tryx::turris_media {

WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath, quint32 kind,
    quint64 frameCount, const std::function<bool()> &isCancelled) {
    WriteResult result;
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        return result;
    }
    if ((kind != kImageKind && kind != kVideoKind) ||
        frameCount == 0 || frameCount > 0xffffffffULL ||
        (kind == kImageKind && frameCount != 1)) {
        result.error = QObject::tr("Turris media attributes are invalid");
        return result;
    }

    const QByteArray metadata = mediaMetadata(kind, frameCount);
    if (metadata.isEmpty() ||
        metadata.size() > static_cast<qsizetype>(0xffffffffU)) {
        result.error = QObject::tr("Turris media metadata is invalid");
        return result;
    }
    QByteArray metadataLength(4, Qt::Uninitialized);
    const quint32 length = static_cast<quint32>(metadata.size());
    metadataLength[0] = static_cast<char>(length & 0xffU);
    metadataLength[1] = static_cast<char>((length >> 8U) & 0xffU);
    metadataLength[2] = static_cast<char>((length >> 16U) & 0xffU);
    metadataLength[3] = static_cast<char>((length >> 24U) & 0xffU);

    const QFileInfo rawInfo(rawPath);
    const qint64 prefixSize = metadataLength.size() + metadata.size();
    if (!rawInfo.exists() || !rawInfo.isFile() || rawInfo.isSymLink() ||
        rawInfo.size() <= 0 ||
        rawInfo.size() > kMaximumBlobBytes - prefixSize) {
        result.error = QObject::tr(
            "Prepared Turris H264 is not a bounded regular file");
        return result;
    }

    QFile rawFile(rawPath);
    if (!rawFile.open(QIODevice::ReadOnly)) {
        result.error = QObject::tr("Cannot open prepared Turris H264: %1")
                           .arg(rawFile.errorString());
        return result;
    }
    QSaveFile outputFile(outputPath);
    outputFile.setDirectWriteFallback(false);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        result.error = QObject::tr("Cannot create Turris media blob: %1")
                           .arg(outputFile.errorString());
        return result;
    }
    if (!outputFile.setPermissions(QFileDevice::ReadOwner |
                                   QFileDevice::WriteOwner)) {
        result.error = QObject::tr(
            "Cannot restrict Turris media blob permissions");
        outputFile.cancelWriting();
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!writeAll(&outputFile, metadataLength) ||
        !writeAll(&outputFile, metadata)) {
        result.error = QObject::tr("Cannot write Turris media metadata: %1")
                           .arg(outputFile.errorString());
        outputFile.cancelWriting();
        return result;
    }
    hash.addData(metadataLength);
    hash.addData(metadata);

    qint64 copied = 0;
    while (!rawFile.atEnd()) {
        if (isCancelled && isCancelled()) {
            result.cancelled = true;
            outputFile.cancelWriting();
            return result;
        }
        const QByteArray chunk = rawFile.read(256 * 1024);
        if (chunk.isEmpty()) {
            if (rawFile.error() != QFileDevice::NoError) {
                result.error = QObject::tr(
                    "Cannot read prepared Turris H264: %1")
                                   .arg(rawFile.errorString());
                outputFile.cancelWriting();
                return result;
            }
            break;
        }
        if (!writeAll(&outputFile, chunk)) {
            result.error = QObject::tr(
                "Cannot write Turris media payload: %1")
                               .arg(outputFile.errorString());
            outputFile.cancelWriting();
            return result;
        }
        hash.addData(chunk);
        copied += chunk.size();
    }
    if (copied != rawInfo.size() ||
        QFileInfo(rawPath).size() != rawInfo.size()) {
        result.error = QObject::tr(
            "Prepared Turris H264 changed while it was wrapped");
        outputFile.cancelWriting();
        return result;
    }
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        outputFile.cancelWriting();
        return result;
    }
    if (!outputFile.flush() || !outputFile.commit()) {
        result.error = QObject::tr("Cannot commit Turris media blob: %1")
                           .arg(outputFile.errorString());
        return result;
    }

    const QFileInfo outputInfo(outputPath);
    if (!outputInfo.exists() || !outputInfo.isFile() ||
        outputInfo.isSymLink() ||
        outputInfo.size() != prefixSize + rawInfo.size()) {
        QFile::remove(outputPath);
        result.error = QObject::tr(
            "Committed Turris media blob failed size verification");
        return result;
    }
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    return result;
}

FrameCountProbeResult parseFrameCountProbe(
    const QByteArray &output, quint32 kind) {
    QHash<QString, QString> probeValues;
    QString probeText = QString::fromLatin1(output);
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
            QString::number(kWidth) &&
        probeValues.value(QStringLiteral("height")) ==
            QString::number(kHeight);

    FrameCountProbeResult result;
    result.valid = (kind == kImageKind || kind == kVideoKind) &&
                   probeShapeValid && probeValues.size() == 3 &&
                   geometryValid && frameCountOk && frameCount != 0 &&
                   frameCount <= 0xffffffffULL &&
                   (kind != kImageKind || frameCount == 1);
    if (result.valid) {
        result.frameCount = frameCount;
    }
    return result;
}

bool validateBlob(QFile *file, qint64 declaredSize,
                  const QString &remoteFileName,
                  QString *errorMessage) {
    const auto reject = [errorMessage](const QString &reason) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Turris media container is invalid: %1")
                                .arg(reason);
        }
        return false;
    };
    if (!file || declaredSize < 8 || !file->seek(0)) {
        return reject(QStringLiteral("container header is unavailable"));
    }

    const QByteArray lengthBytes = file->read(4);
    if (lengthBytes.size() != 4) {
        return reject(QStringLiteral("metadata length is truncated"));
    }
    const auto byteAt = [&lengthBytes](qsizetype index) {
        return static_cast<quint32>(
            static_cast<quint8>(lengthBytes.at(index)));
    };
    const quint32 metadataLength =
        byteAt(0) | (byteAt(1) << 8U) | (byteAt(2) << 16U) |
        (byteAt(3) << 24U);
    const qint64 rawPayloadOffset =
        4 + static_cast<qint64>(metadataLength);
    if (metadataLength == 0 ||
        metadataLength > static_cast<quint32>(kMaximumMetadataBytes) ||
        rawPayloadOffset > declaredSize - 4) {
        return reject(QStringLiteral("metadata length is out of bounds"));
    }

    const QByteArray metadataBytes = file->read(metadataLength);
    if (metadataBytes.size() != static_cast<qsizetype>(metadataLength)) {
        return reject(QStringLiteral("metadata is truncated"));
    }
    MediaMetadata metadata;
    QString metadataError;
    if (!parseMediaMetadata(metadataBytes, &metadata, &metadataError)) {
        return reject(metadataError);
    }

    const QString lowerName = remoteFileName.toLower();
    quint64 expectedKind = 0;
    if (lowerName.endsWith(QStringLiteral(".png.h264_1280x720"))) {
        expectedKind = kImageKind;
    } else if (lowerName.endsWith(
                   QStringLiteral(".mp4.h264_1280x720")) ||
               lowerName.endsWith(
                   QStringLiteral(".gif.h264_1280x720"))) {
        expectedKind = kVideoKind;
    } else {
        return reject(QStringLiteral(
            "file name does not identify media kind"));
    }

    if (metadata.magic != kMediaMagic ||
        metadata.description != QByteArray(kMediaDescription) ||
        metadata.kind != expectedKind ||
        metadata.headerVersion != kHeaderVersion ||
        metadata.framesPerSecond != kFramesPerSecond ||
        metadata.width != kWidth || metadata.height != kHeight ||
        metadata.frameCount == 0 ||
        metadata.frameCount > std::numeric_limits<quint32>::max() ||
        (metadata.kind == kImageKind && metadata.frameCount != 1)) {
        return reject(QStringLiteral(
            "metadata values do not match the profile"));
    }

    const QByteArray rawPrefix = file->read(5);
    const bool startsWithThreeByteAnnexB =
        rawPrefix.size() >= 4 && rawPrefix.at(0) == '\0' &&
        rawPrefix.at(1) == '\0' && rawPrefix.at(2) == '\1';
    const bool startsWithFourByteAnnexB =
        rawPrefix.size() >= 5 && rawPrefix.at(0) == '\0' &&
        rawPrefix.at(1) == '\0' && rawPrefix.at(2) == '\0' &&
        rawPrefix.at(3) == '\1';
    if (!startsWithThreeByteAnnexB && !startsWithFourByteAnnexB) {
        return reject(QStringLiteral(
            "raw payload does not start with an Annex-B NAL unit"));
    }
    if (!file->seek(0)) {
        return reject(QStringLiteral("container cannot be rewound"));
    }
    return true;
}

}  // namespace tryx::turris_media
