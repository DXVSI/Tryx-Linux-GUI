#pragma once

#include "printerframecodec.h"

namespace tryx::printer_protocol_constants {

constexpr qsizetype kFileTransmitChunkSize = 0x40000;
constexpr int kTransferChunkWriteTimeoutMs = 15000;
constexpr int kFileTransmitResponseTimeoutMs = 30000;
constexpr qint64 kMaxMediaUploadSize = 500LL * 1024LL * 1024LL;
constexpr qint64 kMaxMediaPullSize = 500LL * 1024LL * 1024LL;
constexpr int kMaxMediaPullChunks = 16384;
constexpr qint64 kMediaPullDeadlineBaseMs = 30000;
constexpr qint64 kMediaPullDeadlinePerMiBMs = 2000;
constexpr qint64 kMediaPullDeadlineHardLimitMs = 30LL * 60LL * 1000LL;
constexpr qint64 kBytesPerMiB = 1024LL * 1024LL;
constexpr qsizetype kMediaPullCancellationCheckInterval = 64 * 1024;
constexpr quint64 kTurrisTransferTrackId = 981521;
constexpr int kPollCancellationSliceMs = 100;
constexpr int kMaxSkippedResponseFrames = 256;
constexpr qsizetype kMaxSkippedResponseBytes = 4 * 1024 * 1024;
constexpr int kQueuedResponseDrainTimeoutMs = 250;
constexpr int kPrinterKeepaliveIntervalMs = 2000;
constexpr int kPrinterKeepaliveWriteTimeoutMs = 2000;
constexpr int kUdbBootstrapWriteTimeoutMs = 2000;
constexpr int kDeviceInformationReadinessDeadlineMs = 20000;
constexpr int kDeviceInformationReadinessInitialBackoffMs = 500;
constexpr int kDeviceInformationReadinessMaximumBackoffMs = 2000;
constexpr int kIdempotentQueryMaximumAttempts = 2;
constexpr int kIdempotentQueryRetryBackoffMs = 250;
constexpr int kMaxInFlightKeepaliveWriteRetries = 3;
constexpr qsizetype kMaxFrameResynchronizationBytes = 64 * 1024;
constexpr int kLibusbEventSliceMs = 50;
constexpr int kLibusbCancellationDrainTimeoutMs = 2000;
constexpr int kUnrecoverableTransportExitCode = 70;
constexpr int kLibusbInputTransferSize = 64 * 1024;
constexpr int kMaxInputTransferErrorRetries = 10;
constexpr int kInputTransferErrorRearmInitialBackoffMs = 5;
constexpr int kInputTransferErrorRearmMaxBackoffMs = 100;
constexpr int kPersistentInputTransferErrorThreshold = kMaxInputTransferErrorRetries;
constexpr qsizetype kMaxLibusbReceiveQueueSize =
    (PrinterFrameCodec::MaxPayloadSize + 8) * 4;

} // namespace tryx::printer_protocol_constants
