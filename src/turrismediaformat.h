#pragma once

#include <QString>
#include <QByteArray>
#include <QtGlobal>

#include <functional>

class QFile;

namespace tryx::turris_media {

inline constexpr quint16 kProductId = 0x2011;
inline constexpr quint32 kImageKind = 2U;
inline constexpr quint32 kVideoKind = 4U;
inline constexpr quint32 kWidth = 1280U;
inline constexpr quint32 kHeight = 720U;

struct WriteResult {
    QString sha256;
    QString error;
    bool cancelled = false;
};

struct FrameCountProbeResult {
    quint64 frameCount = 0;
    bool valid = false;
};

WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    const std::function<bool()> &isCancelled);

FrameCountProbeResult parseFrameCountProbe(
    const QByteArray &output, quint32 kind);

bool validateBlob(QFile *file, qint64 declaredSize,
                  const QString &remoteFileName,
                  QString *errorMessage);

}  // namespace tryx::turris_media
