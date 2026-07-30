#include "mediatransform.h"

#include <QCryptographicHash>

namespace {

bool failValidation(QString *errorMessage, const QString &message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

bool isNeutralViewport(const TryxRuntimeMediaTransform &transform) {
    return transform.zoomPermille == 1000 &&
           transform.focusX == 5000 &&
           transform.focusY == 5000;
}

QString rotationFilter(quint32 rotationQuarterTurns) {
    switch (rotationQuarterTurns) {
    case 0:
        return {};
    case 1:
        return QStringLiteral("transpose=clock,");
    case 2:
        return QStringLiteral("hflip,vflip,");
    case 3:
        return QStringLiteral("transpose=cclock,");
    default:
        return {};
    }
}

QString squarePixelFilter() {
    return QStringLiteral(
        "scale='if(lte(sar,0),iw,max(1,round(iw*sar)))':ih,"
        "setsar=1,");
}

QString coverViewportFilter(
    const QString &width,
    const QString &height,
    quint32 zoomPermille,
    quint32 focusX,
    quint32 focusY) {
    return QStringLiteral(
               "scale=%1:%2:force_original_aspect_ratio=increase:"
               "force_divisible_by=2,"
               "scale='trunc(iw*%3/1000/2)*2':"
               "'trunc(ih*%3/1000/2)*2',"
               "crop=%1:%2:"
               "'trunc((iw-%1)*%4/10000/2)*2':"
               "'trunc((ih-%2)*%5/10000/2)*2',")
        .arg(width, height)
        .arg(zoomPermille)
        .arg(focusX)
        .arg(focusY);
}

QString outputFilterSuffix() {
    return QStringLiteral("setsar=1,format=yuv420p,fps=30");
}

}  // namespace

TryxRuntimeMediaTransform tryxLegacyFitMediaTransform() {
    return {};
}

bool tryxMediaTransformIsValid(
    const TryxRuntimeMediaTransform &transform,
    QString *errorMessage) {
    if (transform.schemaVersion != 1) {
        return failValidation(
            errorMessage,
            QStringLiteral("Unsupported media transform schema version"));
    }
    if (transform.mode != QStringLiteral("Fit") &&
        transform.mode != QStringLiteral("Fill") &&
        transform.mode != QStringLiteral("Crop") &&
        transform.mode != QStringLiteral("Stretch")) {
        return failValidation(errorMessage,
                              QStringLiteral("Unsupported media transform mode"));
    }
    if (transform.rotationQuarterTurns > 3) {
        return failValidation(
            errorMessage,
            QStringLiteral("Media transform rotation is out of range"));
    }
    if (transform.zoomPermille < 1000 ||
        transform.zoomPermille > 4000) {
        return failValidation(
            errorMessage,
            QStringLiteral("Media transform zoom is out of range"));
    }
    if (transform.focusX > 10000 || transform.focusY > 10000) {
        return failValidation(
            errorMessage,
            QStringLiteral("Media transform focus is out of range"));
    }
    if (transform.backgroundRgb > 0x00FFFFFFU) {
        return failValidation(
            errorMessage,
            QStringLiteral("Media transform background color is out of range"));
    }
    if (transform.mode != QStringLiteral("Crop") &&
        !isNeutralViewport(transform)) {
        return failValidation(
            errorMessage,
            QStringLiteral(
                "Zoom and focus are only supported in Crop mode"));
    }
    if (transform.mode != QStringLiteral("Fit") &&
        transform.backgroundRgb != 0) {
        return failValidation(
            errorMessage,
            QStringLiteral(
                "Background color is only supported in Fit mode"));
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool tryxMediaTransformIsLegacyFit(
    const TryxRuntimeMediaTransform &transform) {
    return tryxMediaTransformIsValid(transform) &&
           transform.schemaVersion == 1 &&
           transform.mode == QStringLiteral("Fit") &&
           transform.rotationQuarterTurns == 0 &&
           transform.zoomPermille == 1000 &&
           transform.focusX == 5000 &&
           transform.focusY == 5000 &&
           transform.backgroundRgb == 0;
}

QString tryxMediaTransformCanonicalValue(
    const TryxRuntimeMediaTransform &transform) {
    if (!tryxMediaTransformIsValid(transform)) {
        return {};
    }
    return QStringLiteral(
               "v=%1;mode=%2;rotation=%3;zoom=%4;focus-x=%5;focus-y=%6;"
               "background=%7")
        .arg(transform.schemaVersion)
        .arg(transform.mode)
        .arg(transform.rotationQuarterTurns)
        .arg(transform.zoomPermille)
        .arg(transform.focusX)
        .arg(transform.focusY)
        .arg(transform.backgroundRgb, 6, 16, QLatin1Char('0'))
        .toLower();
}

QString tryxMediaTransformFingerprint(
    const TryxRuntimeMediaTransform &transform) {
    const QString canonical = tryxMediaTransformCanonicalValue(transform);
    if (canonical.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString tryxMediaTransformFfmpegFilter(
    const TryxRuntimeMediaTransform &transform,
    int targetWidth,
    int targetHeight) {
    if (!tryxMediaTransformIsValid(transform) ||
        targetWidth <= 0 || targetHeight <= 0 ||
        targetWidth % 2 != 0 || targetHeight % 2 != 0) {
        return {};
    }

    const QString width = QString::number(targetWidth);
    const QString height = QString::number(targetHeight);
    QString filter = rotationFilter(transform.rotationQuarterTurns) +
                     squarePixelFilter();

    if (transform.mode == QStringLiteral("Fit")) {
        const QString background =
            QStringLiteral("0x%1")
                .arg(transform.backgroundRgb, 6, 16, QLatin1Char('0'));
        filter += QStringLiteral(
                      "scale=%1:%2:force_original_aspect_ratio=decrease,"
                      "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=%3,")
                      .arg(width, height, background);
    } else if (transform.mode == QStringLiteral("Fill")) {
        filter += coverViewportFilter(
            width, height, 1000, 5000, 5000);
    } else if (transform.mode == QStringLiteral("Stretch")) {
        filter += QStringLiteral("scale=%1:%2,").arg(width, height);
    } else {
        filter += coverViewportFilter(
            width, height,
            transform.zoomPermille,
            transform.focusX,
            transform.focusY);
    }
    filter += outputFilterSuffix();
    return filter;
}
