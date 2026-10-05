#pragma once

#include <QByteArray>
#include <QString>

class PrinterFrameCodec {
public:
    static constexpr qsizetype MaxPayloadSize = 1024 * 1024;

    enum class DecodeStatus { NeedMoreData, FrameReady, Malformed };

    static QByteArray encode(const QByteArray &payload);
    static DecodeStatus takeFrame(QByteArray *buffer, QByteArray *payload,
                                  QString *errorMessage = nullptr);
};
