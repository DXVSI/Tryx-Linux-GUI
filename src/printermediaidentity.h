#pragma once

#include "runtimecontract.h"

#include <QSize>
#include <QString>

struct PrinterProductProfile;

namespace tryx::printer_media_identity {

QString generatedPrinterMediaName(const QString &extension);
QString printerConversionProfile(
    const QString &path,
    const TryxRuntimeMediaTransform &transform,
    quint16 productId);
QString printerConversionProfile(
    const QString &path,
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    quint16 productId);
QString paseRecoveredConversionProfile(
    const QString &transformFingerprint);
QString paseRecoveredConversionProfile(
    const TryxRuntimeMediaPreparationProfileV1 &profile);
QString h264PrinterName(const QString &baseName, quint16 productId);
QString h264PrinterName(
    const QString &baseName, quint16 productId,
    const TryxRuntimeMediaPreparationProfileV1 &profile);
QString h264PrinterNameForConversion(
    const QString &baseName, quint16 productId,
    const QString &conversion);
bool isSafePrinterDeviceMediaName(const QString &fileName);
bool isSafePrinterUploadMediaName(const QString &fileName);
bool isCanonicalPrinterConversionProfile(
    const QString &conversionProfile);
bool printerConversionProfileMatchesMediaName(
    const QString &conversionProfile,
    const QString &fileName);
QString printerMediaConversionIdentity(
    const PrinterProductProfile &profile);
QString printerMediaConversionIdentity(
    const PrinterProductProfile &profile,
    const TryxRuntimeMediaPreparationProfileV1 &preparationProfile);
QSize printerMediaSizeForConversionIdentity(
    const QString &conversion,
    const PrinterProductProfile &profile);
QSize printerMediaSizeForName(
    const QString &fileName,
    const PrinterProductProfile &profile);
bool printerMediaConversionIdentityMatchesProduct(
    const QString &conversion,
    const PrinterProductProfile &profile);
bool printerMediaNameMatchesProfile(
    const QString &fileName,
    const PrinterProductProfile &profile);
bool printerMediaNameMatchesConversion(
    const QString &fileName,
    const PrinterProductProfile &profile,
    const QString &conversion);
bool printerConversionProfileMatchesProduct(
    const QString &conversionProfile,
    const PrinterProductProfile &profile);
bool printerConversionProfileMatchesConversion(
    const QString &conversionProfile,
    const PrinterProductProfile &profile,
    const QString &conversion);

}  // namespace tryx::printer_media_identity
