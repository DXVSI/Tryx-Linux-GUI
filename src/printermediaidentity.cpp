#include "printermediaidentity.h"

#include "mediatransform.h"
#include "printermediafileintegrity.h"
#include "printerprotocol.h"
#include "turrismediaformat.h"

#include <panorama/media.hpp>

#include <QDateTime>
#include <QStringList>
#include <QUuid>

#include <optional>

namespace tryx::printer_media_identity {

namespace {

QString turrisConversionProfileBase(const QString &typeName) {
    if (typeName == QStringLiteral("image")) {
        return QStringLiteral(
            "turris-mxhd-v1-image-single-frame-1280x720-yuv420p-30fps-libx264-main40-medium-crf18");
    }
    return QStringLiteral(
        "turris-mxhd-v1-%1-1280x720-yuv420p-30fps-libx264-main41-fast-12mbps")
        .arg(typeName);
}

QString paseConversionProfileBase(const QString &version,
                                  const QString &typeName) {
    return QStringLiteral(
        "pase-h264-%1-%2-2240x1080-yuv420p-30fps-libx264-veryfast-crf23")
        .arg(version, typeName);
}

QString paseSplitConversionProfileBase(const QString &typeName) {
    return QStringLiteral(
        "pase-h264-v3-%1-split-area-1120x1080-yuv420p-30fps-libx264-veryfast-crf23")
        .arg(typeName);
}

bool splitPreparationSupported(
    const PrinterProductProfile &profile) {
    return profile.mediaUploadSupported &&
           profile.splitAreaMediaSupported &&
           profile.mediaWidth == 2240 &&
           profile.mediaHeight == 1080;
}

bool profileMatchesBaseOrTransform(const QString &profile,
                                   const QString &base) {
    if (profile == base) {
        return true;
    }
    const QString transformPrefix =
        base + QStringLiteral("-transform-");
    return profile.startsWith(transformPrefix) &&
           printer_media_file_integrity::isSha256Hex(
               profile.mid(transformPrefix.size()));
}

}  // namespace

QString generatedPrinterMediaName(const QString &extension) {
    const QString cleanExtension = extension.startsWith(QLatin1Char('.'))
        ? extension.mid(1).toLower()
        : extension.toLower();
    const QString nonce = QUuid::createUuid()
                              .toString(QUuid::WithoutBraces)
                              .left(8);
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") +
           QLatin1Char('-') + nonce + QLatin1Char('.') + cleanExtension;
}

QString printerConversionProfile(
    const QString &path,
    const TryxRuntimeMediaTransform &transform,
    quint16 productId) {
    return printerConversionProfile(
        path, tryxFullFrameMediaPreparationProfile(transform),
        productId);
}

QString printerConversionProfile(
    const QString &path,
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    quint16 productId) {
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(productId);
    if (!productProfile || !productProfile->mediaUploadSupported ||
        !tryxMediaPreparationProfileV1IsValid(profile) ||
        (profile.target == QStringLiteral("SplitArea") &&
         !splitPreparationSupported(*productProfile))) {
        return {};
    }
    const QString transformFingerprint =
        tryxMediaTransformFingerprint(profile.transform);
    if (transformFingerprint.isEmpty()) {
        return {};
    }
    QString typeName;
    switch (panorama::Media::detect_type(path.toStdString())) {
    case panorama::MediaType::Image:
        typeName = productId == turris_media::kProductId
            ? QStringLiteral("image")
            : QStringLiteral("image-60s");
        break;
    case panorama::MediaType::Video:
        typeName = QStringLiteral("video");
        break;
    case panorama::MediaType::Gif:
        typeName = QStringLiteral("gif");
        break;
    case panorama::MediaType::Unknown:
        return {};
    }
    if (profile.target == QStringLiteral("SplitArea")) {
        return paseSplitConversionProfileBase(typeName) +
               QStringLiteral("-transform-") + transformFingerprint;
    }
    if (productId == turris_media::kProductId) {
        const QString base = turrisConversionProfileBase(typeName);
        if (tryxMediaTransformIsLegacyFit(profile.transform)) {
            return base;
        }
        return base + QStringLiteral("-transform-") +
               transformFingerprint;
    }
    if (tryxMediaTransformIsLegacyFit(profile.transform)) {
        return paseConversionProfileBase(QStringLiteral("v1"), typeName);
    }
    return paseConversionProfileBase(QStringLiteral("v2"), typeName) +
           QStringLiteral("-transform-") + transformFingerprint;
}

QString paseRecoveredConversionProfile(
    const QString &transformFingerprint) {
    if (!printer_media_file_integrity::isSha256Hex(
            transformFingerprint)) {
        return {};
    }
    return paseConversionProfileBase(
               QStringLiteral("v2"),
               QStringLiteral("recovered-video")) +
           QStringLiteral("-transform-") + transformFingerprint;
}

QString paseRecoveredConversionProfile(
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (!tryxMediaPreparationProfileV1IsValid(profile)) {
        return {};
    }
    const QString transformFingerprint =
        tryxMediaTransformFingerprint(profile.transform);
    if (transformFingerprint.isEmpty()) {
        return {};
    }
    if (profile.target == QStringLiteral("SplitArea")) {
        return paseSplitConversionProfileBase(
                   QStringLiteral("recovered-video")) +
               QStringLiteral("-transform-") +
               transformFingerprint;
    }
    return paseRecoveredConversionProfile(transformFingerprint);
}

QString h264PrinterName(const QString &baseName, quint16 productId) {
    return h264PrinterName(
        baseName, productId,
        tryxFullFrameMediaPreparationProfile());
}

QString h264PrinterName(
    const QString &baseName, quint16 productId,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(productId);
    if (!productProfile || !productProfile->mediaUploadSupported ||
        !tryxMediaPreparationProfileV1IsValid(profile) ||
        (profile.target == QStringLiteral("SplitArea") &&
         !splitPreparationSupported(*productProfile))) {
        return {};
    }
    const int width = profile.target == QStringLiteral("SplitArea")
        ? productProfile->mediaWidth / 2
        : productProfile->mediaWidth;
    return QStringLiteral("%1.h264_%2x%3")
        .arg(baseName)
        .arg(width)
        .arg(productProfile->mediaHeight);
}

QString h264PrinterNameForConversion(
    const QString &baseName, quint16 productId,
    const QString &conversion) {
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(productId);
    if (!productProfile || !productProfile->mediaUploadSupported) {
        return {};
    }
    const QSize size = printerMediaSizeForConversionIdentity(
        conversion, *productProfile);
    if (!size.isValid()) {
        return {};
    }
    return QStringLiteral("%1.h264_%2x%3")
        .arg(baseName)
        .arg(size.width())
        .arg(size.height());
}

bool isSafePrinterDeviceMediaName(const QString &fileName) {
    if (fileName.isEmpty() || fileName.size() > 128 ||
        fileName.startsWith(QLatin1Char('.')) ||
        fileName.contains(QLatin1Char('/')) ||
        fileName.contains(QLatin1Char('\\'))) {
        return false;
    }

    for (const QChar character : fileName) {
        if (character.isLetterOrNumber() || character == QLatin1Char('.') ||
            character == QLatin1Char('_') || character == QLatin1Char('-')) {
            continue;
        }
        return false;
    }
    return true;
}

bool isSafePrinterUploadMediaName(const QString &fileName) {
    if (!isSafePrinterDeviceMediaName(fileName)) {
        return false;
    }

    const QString lowerName = fileName.toLower();
    for (const QString &suffix : {
             QStringLiteral(".mp4"),
             QStringLiteral(".png"),
             QStringLiteral(".gif"),
             QStringLiteral(".mp4.h264_2240x1080"),
             QStringLiteral(".png.h264_2240x1080"),
             QStringLiteral(".gif.h264_2240x1080"),
             QStringLiteral(".mp4.h264_1120x1080"),
             QStringLiteral(".png.h264_1120x1080"),
             QStringLiteral(".gif.h264_1120x1080"),
             QStringLiteral(".mp4.h264_1280x720"),
             QStringLiteral(".png.h264_1280x720"),
             QStringLiteral(".gif.h264_1280x720"),
         }) {
        if (lowerName.endsWith(suffix)) {
            return true;
        }
    }
    return false;
}

bool isCanonicalPrinterConversionProfile(
    const QString &conversionProfile) {
    for (const QString &typeName : {
             QStringLiteral("image"),
             QStringLiteral("video"),
             QStringLiteral("gif"),
         }) {
        if (profileMatchesBaseOrTransform(
                conversionProfile,
                turrisConversionProfileBase(typeName))) {
            return true;
        }
    }
    for (const QString &typeName : {
             QStringLiteral("image-60s"),
             QStringLiteral("video"),
             QStringLiteral("gif"),
         }) {
        if (conversionProfile ==
            paseConversionProfileBase(QStringLiteral("v1"), typeName)) {
            return true;
        }
        const QString v2Base =
            paseConversionProfileBase(QStringLiteral("v2"), typeName);
        const QString transformPrefix =
            v2Base + QStringLiteral("-transform-");
        if (conversionProfile.startsWith(transformPrefix) &&
            printer_media_file_integrity::isSha256Hex(
                conversionProfile.mid(transformPrefix.size()))) {
            return true;
        }
    }
    const QString recoveredBase =
        paseConversionProfileBase(
            QStringLiteral("v2"),
            QStringLiteral("recovered-video"));
    const QString recoveredTransformPrefix =
        recoveredBase + QStringLiteral("-transform-");
    if (conversionProfile.startsWith(recoveredTransformPrefix) &&
        printer_media_file_integrity::isSha256Hex(
            conversionProfile.mid(recoveredTransformPrefix.size()))) {
        return true;
    }
    for (const QString &typeName : {
             QStringLiteral("image-60s"),
             QStringLiteral("video"),
             QStringLiteral("gif"),
             QStringLiteral("recovered-video"),
         }) {
        const QString splitTransformPrefix =
            paseSplitConversionProfileBase(typeName) +
            QStringLiteral("-transform-");
        if (conversionProfile.startsWith(splitTransformPrefix) &&
            printer_media_file_integrity::isSha256Hex(
                conversionProfile.mid(splitTransformPrefix.size()))) {
            return true;
        }
    }
    return false;
}

bool printerConversionProfileMatchesMediaName(
    const QString &conversionProfile,
    const QString &fileName) {
    if (!isCanonicalPrinterConversionProfile(conversionProfile) ||
        !isSafePrinterUploadMediaName(fileName)) {
        return false;
    }

    QString expectedGeometry;
    if (conversionProfile.startsWith(
            QStringLiteral("turris-mxhd-v1-"))) {
        expectedGeometry = QStringLiteral("1280x720");
    } else if (conversionProfile.startsWith(
                   QStringLiteral("pase-h264-v1-")) ||
               conversionProfile.startsWith(
                   QStringLiteral("pase-h264-v2-"))) {
        expectedGeometry = QStringLiteral("2240x1080");
    } else if (conversionProfile.startsWith(
                   QStringLiteral("pase-h264-v3-")) &&
               conversionProfile.contains(
                   QStringLiteral("-split-area-1120x1080-"))) {
        expectedGeometry = QStringLiteral("1120x1080");
    } else {
        return false;
    }
    return fileName.endsWith(
        QStringLiteral(".h264_%1").arg(expectedGeometry),
        Qt::CaseInsensitive);
}

QString printerMediaConversionIdentity(
    const PrinterProductProfile &profile) {
    if (profile.productId == turris_media::kProductId) {
        return QStringLiteral("turris-mxhd-v1-1280x720-30fps");
    }
    return QStringLiteral("h264-yuv420p-%1x%2-30fps")
        .arg(profile.mediaWidth)
        .arg(profile.mediaHeight);
}

QString printerMediaConversionIdentity(
    const PrinterProductProfile &profile,
    const TryxRuntimeMediaPreparationProfileV1 &preparationProfile) {
    if (!tryxMediaPreparationProfileV1IsValid(preparationProfile)) {
        return {};
    }
    if (preparationProfile.target == QStringLiteral("FullFrame")) {
        return printerMediaConversionIdentity(profile);
    }
    if (!splitPreparationSupported(profile)) {
        return {};
    }
    return QStringLiteral("h264-yuv420p-%1x%2-30fps")
        .arg(profile.mediaWidth / 2)
        .arg(profile.mediaHeight);
}

QSize printerMediaSizeForConversionIdentity(
    const QString &conversion,
    const PrinterProductProfile &profile) {
    if (conversion == printerMediaConversionIdentity(profile)) {
        return QSize(profile.mediaWidth, profile.mediaHeight);
    }
    TryxRuntimeMediaPreparationProfileV1 split;
    split.target = QStringLiteral("SplitArea");
    if (splitPreparationSupported(profile) &&
        conversion == printerMediaConversionIdentity(profile, split)) {
        return QSize(profile.mediaWidth / 2, profile.mediaHeight);
    }
    return {};
}

bool printerMediaConversionIdentityMatchesProduct(
    const QString &conversion,
    const PrinterProductProfile &profile) {
    return printerMediaSizeForConversionIdentity(
               conversion, profile).isValid();
}

QSize printerMediaSizeForName(
    const QString &fileName,
    const PrinterProductProfile &profile) {
    const QString full = printerMediaConversionIdentity(profile);
    if (printerMediaNameMatchesConversion(
            fileName, profile, full)) {
        return printerMediaSizeForConversionIdentity(full, profile);
    }
    TryxRuntimeMediaPreparationProfileV1 split;
    split.target = QStringLiteral("SplitArea");
    const QString splitConversion =
        printerMediaConversionIdentity(profile, split);
    if (!splitConversion.isEmpty() &&
        printerMediaNameMatchesConversion(
            fileName, profile, splitConversion)) {
        return printerMediaSizeForConversionIdentity(
            splitConversion, profile);
    }
    return {};
}

bool printerMediaNameMatchesProfile(
    const QString &fileName,
    const PrinterProductProfile &profile) {
    if (!isSafePrinterUploadMediaName(fileName)) {
        return false;
    }
    if (printerMediaNameMatchesConversion(
            fileName, profile,
            printerMediaConversionIdentity(profile))) {
        return true;
    }
    TryxRuntimeMediaPreparationProfileV1 split;
    split.target = QStringLiteral("SplitArea");
    return splitPreparationSupported(profile) &&
           printerMediaNameMatchesConversion(
               fileName, profile,
               printerMediaConversionIdentity(profile, split));
}

bool printerMediaNameMatchesConversion(
    const QString &fileName,
    const PrinterProductProfile &profile,
    const QString &conversion) {
    const QSize size = printerMediaSizeForConversionIdentity(
        conversion, profile);
    return size.isValid() &&
           isSafePrinterUploadMediaName(fileName) &&
           fileName.endsWith(
               QStringLiteral(".h264_%1x%2")
                   .arg(size.width())
                   .arg(size.height()),
               Qt::CaseInsensitive);
}

bool printerConversionProfileMatchesProduct(
    const QString &conversionProfile,
    const PrinterProductProfile &profile) {
    if (!isCanonicalPrinterConversionProfile(conversionProfile)) {
        return false;
    }
    if (profile.productId == turris_media::kProductId) {
        return conversionProfile.startsWith(
            QStringLiteral("turris-mxhd-v1-"));
    }
    const bool full = conversionProfile.startsWith(
                          QStringLiteral("pase-h264-v1-")) ||
                      conversionProfile.startsWith(
                          QStringLiteral("pase-h264-v2-"));
    if (full) {
        return conversionProfile.contains(
            QStringLiteral("-%1x%2-yuv420p-30fps-")
                .arg(profile.mediaWidth)
                .arg(profile.mediaHeight));
    }
    return splitPreparationSupported(profile) &&
           conversionProfile.startsWith(
               QStringLiteral("pase-h264-v3-")) &&
           conversionProfile.contains(
               QStringLiteral("-split-area-%1x%2-yuv420p-30fps-")
                   .arg(profile.mediaWidth / 2)
                   .arg(profile.mediaHeight));
}

bool printerConversionProfileMatchesConversion(
    const QString &conversionProfile,
    const PrinterProductProfile &profile,
    const QString &conversion) {
    if (!printerConversionProfileMatchesProduct(
            conversionProfile, profile)) {
        return false;
    }
    const QSize size = printerMediaSizeForConversionIdentity(
        conversion, profile);
    if (!size.isValid()) {
        return false;
    }
    if (size.width() == profile.mediaWidth) {
        return !conversionProfile.startsWith(
            QStringLiteral("pase-h264-v3-"));
    }
    return conversionProfile.startsWith(
        QStringLiteral("pase-h264-v3-"));
}

}  // namespace tryx::printer_media_identity
