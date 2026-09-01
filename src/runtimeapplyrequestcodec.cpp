#include "runtimeapplyrequestcodec.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

namespace {

QJsonArray stringListToJson(const QStringList &values) {
    return QJsonArray::fromStringList(values);
}

bool stringListFromJson(const QJsonObject &object, const QString &key,
                        QStringList *values) {
    if (!values || !object.value(key).isArray()) {
        return false;
    }
    QStringList parsed;
    const QJsonArray array = object.value(key).toArray();
    parsed.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isString()) {
            return false;
        }
        parsed.append(value.toString());
    }
    *values = parsed;
    return true;
}

}  // namespace

namespace tryx::runtime_apply_request_codec {

QJsonObject runtimeApplyRequestToJson(
    const TryxRuntimeApplyRequest &request) {
    QJsonObject display;
    display.insert(QStringLiteral("brightnessPresent"),
                   request.display.brightnessPresent);
    display.insert(QStringLiteral("brightness"),
                   request.display.brightness);
    display.insert(QStringLiteral("standbyPresent"),
                   request.display.standbyPresent);
    display.insert(QStringLiteral("standbyEnabled"),
                   request.display.standbyEnabled);
    display.insert(QStringLiteral("backlightPresent"),
                   request.display.backlightPresent);
    display.insert(QStringLiteral("backlightEnabled"),
                   request.display.backlightEnabled);
    display.insert(QStringLiteral("orientationPresent"),
                   request.display.orientationPresent);
    display.insert(QStringLiteral("mirrorMode"),
                   request.display.mirrorMode);
    display.insert(QStringLiteral("waterfallMode"),
                   request.display.waterfallMode);

    QJsonObject object;
    object.insert(QStringLiteral("media"),
                  stringListToJson(request.media));
    object.insert(QStringLiteral("ratio"), request.ratio);
    object.insert(QStringLiteral("screenMode"), request.screenMode);
    object.insert(QStringLiteral("playMode"), request.playMode);
    object.insert(QStringLiteral("sysinfoLabels"),
                  stringListToJson(request.sysinfoLabels));
    object.insert(QStringLiteral("settingsPosition"),
                  request.settingsPosition);
    object.insert(QStringLiteral("settingsColor"),
                  request.settingsColor);
    object.insert(QStringLiteral("settingsAlign"),
                  request.settingsAlign);
    object.insert(QStringLiteral("settingsBadges"),
                  stringListToJson(request.settingsBadges));
    object.insert(QStringLiteral("filterOpacity"),
                  request.filterOpacity);
    object.insert(QStringLiteral("presetId"), request.presetId);
    object.insert(QStringLiteral("sysinfoLabels2"),
                  stringListToJson(request.sysinfoLabels2));
    object.insert(QStringLiteral("settingsBadges2"),
                  stringListToJson(request.settingsBadges2));
    object.insert(QStringLiteral("settingsPosition2"),
                  request.settingsPosition2);
    object.insert(QStringLiteral("settingsColor2"),
                  request.settingsColor2);
    object.insert(QStringLiteral("settingsAlign2"),
                  request.settingsAlign2);
    object.insert(QStringLiteral("waterfallMode"),
                  request.waterfallMode);
    object.insert(QStringLiteral("replaceOverlay"),
                  request.replaceOverlay);
    object.insert(QStringLiteral("display"), display);
    return object;
}

QString runtimeApplyRequestFingerprint(
    const TryxRuntimeApplyRequest &request) {
    const QByteArray canonical =
        QJsonDocument(runtimeApplyRequestToJson(request))
            .toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(
            canonical, QCryptographicHash::Sha256).toHex());
}

QString runtimeMediaTransformRequestFingerprint(
    const TryxRuntimeMediaTransform &transform) {
    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"),
                  static_cast<qint64>(transform.schemaVersion));
    object.insert(QStringLiteral("mode"), transform.mode);
    object.insert(QStringLiteral("rotationQuarterTurns"),
                  static_cast<qint64>(transform.rotationQuarterTurns));
    object.insert(QStringLiteral("zoomPermille"),
                  static_cast<qint64>(transform.zoomPermille));
    object.insert(QStringLiteral("focusX"),
                  static_cast<qint64>(transform.focusX));
    object.insert(QStringLiteral("focusY"),
                  static_cast<qint64>(transform.focusY));
    object.insert(QStringLiteral("backgroundRgb"),
                  static_cast<qint64>(transform.backgroundRgb));
    const QByteArray canonical =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(
            canonical, QCryptographicHash::Sha256).toHex());
}

bool runtimeApplyRequestFromJson(
    const QJsonObject &object, TryxRuntimeApplyRequest *request,
    bool requireBacklightFields) {
    if (!request ||
        !object.value(QStringLiteral("ratio")).isString() ||
        !object.value(QStringLiteral("screenMode")).isString() ||
        !object.value(QStringLiteral("playMode")).isString() ||
        !object.value(QStringLiteral("settingsPosition")).isString() ||
        !object.value(QStringLiteral("settingsColor")).isString() ||
        !object.value(QStringLiteral("settingsAlign")).isString() ||
        !object.value(QStringLiteral("filterOpacity")).isDouble() ||
        !object.value(QStringLiteral("presetId")).isString() ||
        !object.value(QStringLiteral("settingsPosition2")).isString() ||
        !object.value(QStringLiteral("settingsColor2")).isString() ||
        !object.value(QStringLiteral("settingsAlign2")).isString() ||
        !object.value(QStringLiteral("waterfallMode")).isBool() ||
        !object.value(QStringLiteral("replaceOverlay")).isBool() ||
        !object.value(QStringLiteral("display")).isObject()) {
        return false;
    }

    TryxRuntimeApplyRequest parsed;
    if (!stringListFromJson(
            object, QStringLiteral("media"), &parsed.media) ||
        !stringListFromJson(
            object, QStringLiteral("sysinfoLabels"),
            &parsed.sysinfoLabels) ||
        !stringListFromJson(
            object, QStringLiteral("settingsBadges"),
            &parsed.settingsBadges) ||
        !stringListFromJson(
            object, QStringLiteral("sysinfoLabels2"),
            &parsed.sysinfoLabels2) ||
        !stringListFromJson(
            object, QStringLiteral("settingsBadges2"),
            &parsed.settingsBadges2)) {
        return false;
    }
    parsed.ratio = object.value(QStringLiteral("ratio")).toString();
    parsed.screenMode =
        object.value(QStringLiteral("screenMode")).toString();
    parsed.playMode =
        object.value(QStringLiteral("playMode")).toString();
    parsed.settingsPosition =
        object.value(QStringLiteral("settingsPosition")).toString();
    parsed.settingsColor =
        object.value(QStringLiteral("settingsColor")).toString();
    parsed.settingsAlign =
        object.value(QStringLiteral("settingsAlign")).toString();
    parsed.filterOpacity =
        object.value(QStringLiteral("filterOpacity")).toInt();
    parsed.presetId =
        object.value(QStringLiteral("presetId")).toString();
    parsed.settingsPosition2 =
        object.value(QStringLiteral("settingsPosition2")).toString();
    parsed.settingsColor2 =
        object.value(QStringLiteral("settingsColor2")).toString();
    parsed.settingsAlign2 =
        object.value(QStringLiteral("settingsAlign2")).toString();
    parsed.waterfallMode =
        object.value(QStringLiteral("waterfallMode")).toBool();
    parsed.replaceOverlay =
        object.value(QStringLiteral("replaceOverlay")).toBool();

    const QJsonObject display =
        object.value(QStringLiteral("display")).toObject();
    if (!display.value(QStringLiteral("brightnessPresent")).isBool() ||
        !display.value(QStringLiteral("brightness")).isDouble() ||
        !display.value(QStringLiteral("standbyPresent")).isBool() ||
        !display.value(QStringLiteral("standbyEnabled")).isBool() ||
        !display.value(QStringLiteral("orientationPresent")).isBool() ||
        !display.value(QStringLiteral("mirrorMode")).isBool() ||
        !display.value(QStringLiteral("waterfallMode")).isBool()) {
        return false;
    }
    const QJsonValue backlightPresent =
        display.value(QStringLiteral("backlightPresent"));
    const QJsonValue backlightEnabled =
        display.value(QStringLiteral("backlightEnabled"));
    if ((requireBacklightFields &&
         (!backlightPresent.isBool() ||
          !backlightEnabled.isBool())) ||
        (!backlightPresent.isUndefined() &&
         !backlightPresent.isBool()) ||
        (!backlightEnabled.isUndefined() &&
         !backlightEnabled.isBool())) {
        return false;
    }
    parsed.display.brightnessPresent =
        display.value(QStringLiteral("brightnessPresent")).toBool();
    parsed.display.brightness =
        display.value(QStringLiteral("brightness")).toInt();
    parsed.display.standbyPresent =
        display.value(QStringLiteral("standbyPresent")).toBool();
    parsed.display.standbyEnabled =
        display.value(QStringLiteral("standbyEnabled")).toBool();
    parsed.display.backlightPresent =
        backlightPresent.toBool(false);
    parsed.display.backlightEnabled =
        backlightEnabled.toBool(true);
    parsed.display.orientationPresent =
        display.value(QStringLiteral("orientationPresent")).toBool();
    parsed.display.mirrorMode =
        display.value(QStringLiteral("mirrorMode")).toBool();
    parsed.display.waterfallMode =
        display.value(QStringLiteral("waterfallMode")).toBool();
    if (parsed.settingsColor.isEmpty()) {
        parsed.settingsColor = QStringLiteral("#dcdcdc");
    }
    if (parsed.settingsColor2.isEmpty()) {
        parsed.settingsColor2 = parsed.settingsColor;
    }
    *request = parsed;
    return true;
}

}  // namespace tryx::runtime_apply_request_codec
