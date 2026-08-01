#pragma once

#include "runtimecontract.h"

#include <QString>

constexpr int kTryxMediaTargetWidth = 2240;
constexpr int kTryxMediaTargetHeight = 1080;

TryxRuntimeMediaTransform tryxLegacyFitMediaTransform();
bool tryxMediaTransformIsValid(
    const TryxRuntimeMediaTransform &transform,
    QString *errorMessage = nullptr);
bool tryxMediaTransformIsLegacyFit(
    const TryxRuntimeMediaTransform &transform);
QString tryxMediaTransformCanonicalValue(
    const TryxRuntimeMediaTransform &transform);
QString tryxMediaTransformFingerprint(
    const TryxRuntimeMediaTransform &transform);
QString tryxMediaTransformFfmpegFilter(
    const TryxRuntimeMediaTransform &transform,
    int targetWidth = kTryxMediaTargetWidth,
    int targetHeight = kTryxMediaTargetHeight);
