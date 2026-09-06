#pragma once

#include "runtimecontract.h"

#include <QJsonObject>
#include <QString>

namespace tryx::runtime_apply_request_codec {

QJsonObject runtimeApplyWithBadgesV1ToJson(const TryxRuntimeApplyWithBadgesV1 &request);
QString runtimeApplyWithBadgesV1Fingerprint(const TryxRuntimeApplyWithBadgesV1 &request);
bool runtimeApplyWithBadgesV1FromJson(const QJsonObject &object, TryxRuntimeApplyWithBadgesV1 *request);

QJsonObject runtimeApplyRequestToJson(
    const TryxRuntimeApplyRequest &request);
QString runtimeApplyRequestFingerprint(
    const TryxRuntimeApplyRequest &request);
QString runtimeMediaTransformRequestFingerprint(
    const TryxRuntimeMediaTransform &transform);
bool runtimeApplyRequestFromJson(
    const QJsonObject &object, TryxRuntimeApplyRequest *request,
    bool requireBacklightFields);

}  // namespace tryx::runtime_apply_request_codec
