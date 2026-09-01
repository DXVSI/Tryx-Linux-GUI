#pragma once

#include "runtimecontract.h"

#include <QJsonObject>
#include <QString>

namespace tryx::runtime_apply_request_codec {

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
