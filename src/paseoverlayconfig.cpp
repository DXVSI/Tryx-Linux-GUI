#include "paseoverlayconfig.h"

#include <QColor>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {

QString normalizedAlignment(const QString &alignment) {
    return tryx::pase_overlay_config::isSupportedPaseAlignment(
               alignment)
        ? alignment
        : QStringLiteral("Left");
}

bool parseTextColor(const QString &color, quint32 *value) {
    if (!value) {
        return false;
    }
    if (color.size() != 7 || color.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (qsizetype index = 1; index < color.size(); ++index) {
        const QChar character = color.at(index);
        const bool hexadecimal =
            character.isDigit() ||
            (character >= QLatin1Char('a') &&
             character <= QLatin1Char('f')) ||
            (character >= QLatin1Char('A') &&
             character <= QLatin1Char('F'));
        if (!hexadecimal) {
            return false;
        }
    }
    bool ok = false;
    const quint32 parsed = color.mid(1).toUInt(&ok, 16);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

QString normalizedVerticalPlacement(const QString &position) {
    return tryx::pase_overlay_config::
               isSupportedPaseVerticalPlacement(position)
        ? position
        : QStringLiteral("Top");
}

}  // namespace

namespace tryx::pase_overlay_config {

QString printerPresetMediaFile(const QString &presetId) {
    if (presetId == QStringLiteral("Pre-set 1: Cooling delivery")) {
        return QStringLiteral("default_01.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 2: Migration")) {
        return QStringLiteral("default_02.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 3: Quantum time capsule")) {
        return QStringLiteral("default_04.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 4: Exo-Ecologies")) {
        return QStringLiteral("default_03.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 5: Racing")) {
        return QStringLiteral("default_05.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 6: Shuttle")) {
        return QStringLiteral("default_06.mp4.h264_2240x1080");
    }
    return {};
}

QString printerMediaConfigName(const QString &mediaFile) {
    const QString trimmed = mediaFile.trimmed();
    if (trimmed.startsWith(QStringLiteral("/userdata/")) ||
        trimmed.startsWith(QStringLiteral("/sdcard/pcMedia/"))) {
        return QFileInfo(trimmed).fileName();
    }
    return trimmed;
}

bool isSupportedPaseMetricLabel(const QString &label) {
    static const QSet<QString> supported{
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("CPU Usage"),
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Frequency"),
        QStringLiteral("GPU Usage"),
        QStringLiteral("GPU Power"),
        QStringLiteral("Memory Frequency"),
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time"),
    };
    return supported.contains(label);
}

bool hasDuplicateMetricLabels(const QStringList &labels) {
    QSet<QString> seen;
    for (const QString &label : labels) {
        if (seen.contains(label)) {
            return true;
        }
        seen.insert(label);
    }
    return false;
}

bool isSupportedPaseBadge(const QString &badge) {
    return badge == QStringLiteral("CPU Badge") ||
           badge == QStringLiteral("GPU Badge");
}

bool hasDuplicateValues(const QStringList &values) {
    QSet<QString> seen;
    for (const QString &value : values) {
        if (seen.contains(value)) {
            return true;
        }
        seen.insert(value);
    }
    return false;
}

bool paseOverlayHasMetrics(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return !overlay.left.metrics.isEmpty() ||
           (overlay.dualMode && !overlay.right.metrics.isEmpty());
}

bool paseOverlayHasContent(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return paseOverlayHasMetrics(overlay) ||
           !overlay.left.badges.isEmpty() ||
           (overlay.dualMode && !overlay.right.badges.isEmpty());
}

QString paseTextColorName(quint32 color) {
    return QColor::fromRgb(color & 0x00FFFFFFU).name(QColor::HexRgb);
}

bool isValidPaseTextColor(const QString &color) {
    quint32 value = 0;
    return parseTextColor(color, &value);
}

bool isSupportedPaseAlignment(const QString &alignment) {
    return alignment == QStringLiteral("Left") ||
           alignment == QStringLiteral("Center") ||
           alignment == QStringLiteral("Right");
}

bool isSupportedPaseVerticalPlacement(const QString &position) {
    return position == QStringLiteral("Top") ||
           position == QStringLiteral("Bottom");
}

bool isValidPaseOverlayStyle(
    const QString &position, const QString &color,
    const QString &alignment) {
    return isSupportedPaseVerticalPlacement(position) &&
           isValidPaseTextColor(color) &&
           isSupportedPaseAlignment(alignment);
}

bool normalizeAndValidatePaseApplyOverlayStyles(
    TryxRuntimeApplyRequest *request) {
    if (!request) {
        return false;
    }
    if (!request->replaceOverlay) {
        return true;
    }
    if (request->settingsPosition.isEmpty()) {
        request->settingsPosition = QStringLiteral("Top");
    }
    if (request->settingsColor.isEmpty()) {
        request->settingsColor = QStringLiteral("#dcdcdc");
    }
    if (request->settingsAlign.isEmpty()) {
        request->settingsAlign = QStringLiteral("Left");
    }
    if (!isValidPaseOverlayStyle(
            request->settingsPosition,
            request->settingsColor,
            request->settingsAlign)) {
        return false;
    }
    if (request->screenMode !=
        QStringLiteral("Screen Splitting")) {
        return true;
    }
    if (request->settingsPosition2.isEmpty()) {
        request->settingsPosition2 = request->settingsPosition;
    }
    if (request->settingsColor2.isEmpty()) {
        request->settingsColor2 = request->settingsColor;
    }
    if (request->settingsAlign2.isEmpty()) {
        request->settingsAlign2 = request->settingsAlign;
    }
    return isValidPaseOverlayStyle(
        request->settingsPosition2,
        request->settingsColor2,
        request->settingsAlign2);
}

PrinterProtocol::PaseOverlayConfig paseOverlayFromApplyRequest(
    const TryxRuntimeApplyRequest &request) {
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = request.sysinfoLabels;
    overlay.left.badges = request.settingsBadges;
    overlay.left.alignment = normalizedAlignment(request.settingsAlign);
    parseTextColor(request.settingsColor, &overlay.left.textColor);
    overlay.left.verticalPlacement =
        normalizedVerticalPlacement(request.settingsPosition);
    overlay.dualMode =
        request.screenMode == QStringLiteral("Screen Splitting");
    overlay.waterfallMode = request.display.orientationPresent
        ? request.display.waterfallMode
        : request.waterfallMode;
    if (overlay.dualMode) {
        overlay.right.metrics = request.sysinfoLabels2;
        overlay.right.badges = request.settingsBadges2;
        overlay.right.alignment = normalizedAlignment(
            request.settingsAlign2.isEmpty()
                ? request.settingsAlign
                : request.settingsAlign2);
        parseTextColor(
            request.settingsColor2.isEmpty()
                ? request.settingsColor
                : request.settingsColor2,
            &overlay.right.textColor);
        overlay.right.verticalPlacement = normalizedVerticalPlacement(
            request.settingsPosition2.isEmpty()
                ? request.settingsPosition
                : request.settingsPosition2);
    }
    return overlay;
}

PrinterProtocol::PaseOverlayConfig paseOverlayFromMetricsRequest(
    const TryxRuntimeMetricsConfigRequest &request) {
    PrinterProtocol::PaseOverlayConfig overlay;
    if (request.enabled) {
        overlay.left.metrics = request.metrics;
    }
    overlay.left.alignment = request.alignment;
    overlay.left.textColor = request.textColor;
    return overlay;
}

bool paseUploadApplyRequestIsValid(
    const TryxRuntimeApplyRequest &request) {
    TryxRuntimeApplyRequest normalized = request;
    if (!normalizeAndValidatePaseApplyOverlayStyles(
            &normalized)) {
        return false;
    }
    const auto metricsAreValid = [](const QStringList &metrics) {
        return metrics.size() <= 3 &&
               !hasDuplicateMetricLabels(metrics) &&
               std::all_of(
                   metrics.cbegin(), metrics.cend(),
                   [](const QString &label) {
                       return isSupportedPaseMetricLabel(label);
                   });
    };
    const auto badgesAreValid = [](const QStringList &badges) {
        return badges.size() <= 2 && !hasDuplicateValues(badges) &&
               std::all_of(
                   badges.cbegin(), badges.cend(),
                   [](const QString &badge) {
                       return isSupportedPaseBadge(badge);
                   });
    };
    return normalized.media.isEmpty() &&
           normalized.screenMode == QStringLiteral("Full Screen") &&
           (normalized.playMode == QStringLiteral("Single") ||
            normalized.playMode == QStringLiteral("Loop") ||
            normalized.playMode == QStringLiteral("Shuffle")) &&
           normalized.ratio == QStringLiteral("2:1") &&
           metricsAreValid(normalized.sysinfoLabels) &&
           badgesAreValid(normalized.settingsBadges) &&
           normalized.sysinfoLabels2.isEmpty() &&
           normalized.settingsBadges2.isEmpty() &&
           !normalized.display.standbyPresent &&
           (!normalized.display.brightnessPresent ||
            (normalized.display.brightness >= 0 &&
             normalized.display.brightness <= 100));
}

bool paseOverlayRequestsBadge(
    const PrinterProtocol::PaseOverlayConfig &overlay,
    const QString &badge) {
    return overlay.left.badges.contains(badge) ||
           (overlay.dualMode && overlay.right.badges.contains(badge));
}

}  // namespace tryx::pase_overlay_config
