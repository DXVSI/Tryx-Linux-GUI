#pragma once

#include "printerframecodec.h"

namespace tryx::printer_frame_codec {
qsizetype discardBytesBeforeFrameMagic(QByteArray *buffer);
quint32 framePayloadSize(const QByteArray &buffer);
qsizetype completePlausibleFrameIndex(const QByteArray &buffer);
qsizetype discardBytesBeforePlausibleFrame(QByteArray *buffer);
} // namespace tryx::printer_frame_codec
