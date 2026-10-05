#pragma once

#include "printerprotocol.h"
#include "runtimecontract.h"

#include <QString>
#include <QStringList>

namespace tryx::pase_overlay_config {

QString printerPresetMediaFile(const QString &presetId);
QString printerMediaConfigName(const QString &mediaFile);
bool isSupportedPaseMetricLabel(const QString &label);
bool hasDuplicateMetricLabels(const QStringList &labels);
bool isSupportedPaseBadge(const QString &badge);
bool hasDuplicateValues(const QStringList &values);
bool paseOverlayHasMetrics(
    const PrinterProtocol::PaseOverlayConfig &overlay);
bool paseOverlayHasContent(
    const PrinterProtocol::PaseOverlayConfig &overlay);
QString paseTextColorName(quint32 color);
bool isValidPaseTextColor(const QString &color);
bool isSupportedPaseAlignment(const QString &alignment);
bool isSupportedPaseVerticalPlacement(const QString &position);
bool isValidPaseOverlayStyle(
    const QString &position, const QString &color,
    const QString &alignment);
bool normalizeAndValidatePaseApplyOverlayStyles(
    TryxRuntimeApplyRequest *request);
PrinterProtocol::PaseOverlayConfig paseOverlayFromApplyRequest(
    const TryxRuntimeApplyRequest &request);
PrinterProtocol::PaseOverlayConfig paseOverlayFromMetricsRequest(
    const TryxRuntimeMetricsConfigRequest &request);
bool paseUploadApplyRequestIsValid(
    const TryxRuntimeApplyRequest &request);
bool paseBadgeUploadContinuationIsValid(const TryxRuntimeApplyWithBadgesV1 &envelope,
                                       quint16 productId);
bool paseOverlayRequestsBadge(
    const PrinterProtocol::PaseOverlayConfig &overlay,
    const QString &badge);
bool paseBadgeChoicesAreValid(const PrinterProtocol::PaseOverlayConfig &overlay,
                              quint16 productId, QString *error = nullptr);
// Product-level overlay feature gates: overlay metrics at all, split-screen
// (dual) areas and waterfall orientation.
bool paseOverlayIsSupportedByProduct(const PrinterProtocol::PaseOverlayConfig &overlay,
                                     quint16 productId, QString *error = nullptr);
bool paseMetricsRequestIsSupportedByProduct(
    const TryxRuntimeMetricsConfigRequest &request, quint16 productId,
    QString *error = nullptr);

}  // namespace tryx::pase_overlay_config
