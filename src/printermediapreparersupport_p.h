#pragma once

#include "runtimecontract.h"

#include <QString>

#include <functional>

namespace tryx::printer_media_preparer_support {

extern const qint64 kMaxRetryCacheBytes;
extern const quint16 kTurrisProductId;
extern const quint32 kTurrisImageKind;
extern const quint32 kTurrisVideoKind;
extern const quint32 kTurrisMediaWidth;
extern const quint32 kTurrisMediaHeight;

struct SafeSourceHashResult {
    QString sha256;
    qint64 size = 0;
    QString error;
    bool cancelled = false;
};

struct TurrisMediaBlobResult {
    QString sha256;
    QString error;
    bool cancelled = false;
};

QString generatedPrinterMediaName(const QString &extension);
QString printerTempPath(const QString &fileName);
QString sha256File(
    const QString &path,
    const std::function<bool()> &isCancelled = {});
SafeSourceHashResult hashRegularSourceFile(
    const QString &path,
    const std::function<bool()> &isCancelled);
bool isSha256Hex(const QString &value);
QString printerConversionProfile(
    const QString &path,
    const TryxRuntimeMediaTransform &transform,
    quint16 productId);
QString h264PrinterName(const QString &baseName, quint16 productId);
TurrisMediaBlobResult writeTurrisMediaBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    const std::function<bool()> &isCancelled);

}  // namespace tryx::printer_media_preparer_support
