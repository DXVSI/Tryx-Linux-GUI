#pragma once

#include "runtimecontract.h"

#include <QString>

constexpr int kTryxMediaTargetWidth = 2240;
constexpr int kTryxMediaTargetHeight = 1080;
constexpr int kTryxMediaSplitTargetWidth = 1120;
constexpr int kTryxMediaSplitTargetHeight = 1080;

TryxRuntimeMediaTransform tryxLegacyFitMediaTransform();
TryxRuntimeMediaPreparationProfileV1
tryxFullFrameMediaPreparationProfile(
    const TryxRuntimeMediaTransform &transform =
        TryxRuntimeMediaTransform{});
bool tryxMediaPreparationProfileV1IsValid(
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    QString *errorMessage = nullptr);
int tryxMediaPreparationTargetWidth(
    const TryxRuntimeMediaPreparationProfileV1 &profile);
int tryxMediaPreparationTargetHeight(
    const TryxRuntimeMediaPreparationProfileV1 &profile);
QString tryxMediaPreparationProfileCanonicalValue(
    const TryxRuntimeMediaPreparationProfileV1 &profile);
QString tryxMediaPreparationProfileFingerprint(
    const TryxRuntimeMediaPreparationProfileV1 &profile);
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
