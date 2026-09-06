#include "printerframecodec_p.h"
#include <QObject>

namespace tryx::printer_frame_codec {
qsizetype discardBytesBeforeFrameMagic(QByteArray *buffer) {
    static const QByteArray magic = QByteArrayLiteral("TRYX");
    if (!buffer || buffer->isEmpty() || buffer->startsWith(magic)) {
        return 0;
    }

    const qsizetype magicIndex = buffer->indexOf(magic);
    if (magicIndex > 0) {
        buffer->remove(0, magicIndex);
        return magicIndex;
    }

    qsizetype preservedSuffix = qMin<qsizetype>(magic.size() - 1, buffer->size());
    while (preservedSuffix > 0 &&
           buffer->right(preservedSuffix) != magic.left(preservedSuffix)) {
        --preservedSuffix;
    }
    const qsizetype discardedBytes = buffer->size() - preservedSuffix;
    buffer->remove(0, discardedBytes);
    return discardedBytes;
}

quint32 framePayloadSize(const QByteArray &buffer) {
    return static_cast<quint8>(buffer.at(4)) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(5))) << 8) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(6))) << 16) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(7))) << 24);
}

qsizetype completePlausibleFrameIndex(const QByteArray &buffer) {
    static const QByteArray magic = QByteArrayLiteral("TRYX");
    qsizetype index = buffer.indexOf(magic);
    while (index >= 0) {
        const qsizetype remaining = buffer.size() - index;
        if (remaining >= 8) {
            const QByteArray header = buffer.mid(index, 8);
            const quint32 payloadSize = framePayloadSize(header);
            if (payloadSize <=
                    static_cast<quint32>(PrinterFrameCodec::MaxPayloadSize) &&
                remaining >= 8 + static_cast<qsizetype>(payloadSize)) {
                return index;
            }
        }
        index = buffer.indexOf(magic, index + 1);
    }
    return -1;
}

qsizetype discardBytesBeforePlausibleFrame(QByteArray *buffer) {
    qsizetype discardedBytes = 0;
    while (buffer && !buffer->isEmpty()) {
        discardedBytes += discardBytesBeforeFrameMagic(buffer);
        if (buffer->size() < 8) {
            return discardedBytes;
        }
        if (framePayloadSize(*buffer) <=
            static_cast<quint32>(PrinterFrameCodec::MaxPayloadSize)) {
            return discardedBytes;
        }

        // Protobuf strings may legitimately contain the ASCII bytes "TRYX".
        // If a damaged preceding frame leaves such a string at the front of
        // the stream, its following text must not be accepted as a frame
        // length. Drop one byte and continue the bounded magic search so a
        // later real header can still be recovered.
        buffer->remove(0, 1);
        ++discardedBytes;
    }
    return discardedBytes;
}

} // namespace tryx::printer_frame_codec

QByteArray PrinterFrameCodec::encode(const QByteArray &payload) {
    if (payload.size() > MaxPayloadSize) {
        return {};
    }

    QByteArray frame;
    frame.reserve(8 + payload.size());
    frame.append("TRYX", 4);
    const quint32 size = static_cast<quint32>(payload.size());
    frame.append(static_cast<char>(size & 0xff));
    frame.append(static_cast<char>((size >> 8) & 0xff));
    frame.append(static_cast<char>((size >> 16) & 0xff));
    frame.append(static_cast<char>((size >> 24) & 0xff));
    frame.append(payload);
    return frame;
}

PrinterFrameCodec::DecodeStatus PrinterFrameCodec::takeFrame(QByteArray *buffer,
                                                             QByteArray *payload,
                                                             QString *errorMessage) {
    if (!buffer) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX frame buffer is not available");
        }
        return DecodeStatus::Malformed;
    }
    if (buffer->size() < 4) {
        return DecodeStatus::NeedMoreData;
    }
    if (!buffer->startsWith("TRYX")) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX response has invalid frame magic");
        }
        buffer->clear();
        return DecodeStatus::Malformed;
    }
    if (buffer->size() < 8) {
        return DecodeStatus::NeedMoreData;
    }

    const quint32 size =
        static_cast<quint8>(buffer->at(4)) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(5))) << 8) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(6))) << 16) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(7))) << 24);
    if (size > static_cast<quint32>(MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX response payload is too large: %1 bytes").arg(size);
        }
        buffer->clear();
        return DecodeStatus::Malformed;
    }

    const qsizetype frameSize = 8 + static_cast<qsizetype>(size);
    if (buffer->size() < frameSize) {
        return DecodeStatus::NeedMoreData;
    }
    if (payload) {
        *payload = buffer->mid(8, static_cast<qsizetype>(size));
    }
    buffer->remove(0, frameSize);
    return DecodeStatus::FrameReady;
}
