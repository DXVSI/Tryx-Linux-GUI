#pragma once

#include <QString>
#include <QByteArray>
#include <QtGlobal>

#include <functional>

class QFile;

namespace tryx::turris_media {

inline constexpr quint16 kProductId = 0x2011;
// MediaHeaderPb.original_media_type: PNG=2, GIF=3, MP4=4. Still images are
// always delivered as PNG-derived single frames, matching the official app.
inline constexpr quint32 kImageKind = 2U;
inline constexpr quint32 kGifKind = 3U;
// The official converter overlays a still on its background with
// shortest=1, so a still image is exactly one encoded frame.
inline constexpr quint64 kStillImageFrames = 1ULL;
inline constexpr quint32 kVideoKind = 4U;
inline constexpr quint32 kWidth = 1280U;
inline constexpr quint32 kHeight = 720U;

// Frame rate the official application encodes and declares per media kind:
// stills at 30 fps, video and animated GIF at 60 fps.
quint32 framesPerSecondForKind(quint32 kind);

// Target H.264 bitrate in kbit/s, reproducing the official converter: the
// source bitrate scaled by the frame-rate ratio (clamped to 0.75..2.0) and
// the frame-area ratio (clamped to 0.35..1.0) plus 15 percent, bounded to
// 500..12000 kbit/s; 4000 kbit/s when the source bitrate is unknown.
int videoBitrateKbps(qint64 sourceKbps, double sourceFps, int sourceWidth,
                     int sourceHeight, int outputWidth, int outputHeight,
                     int outputFps);

struct WriteResult {
    QString sha256;
    QString error;
    bool cancelled = false;
};

struct FrameCountProbeResult {
    quint64 frameCount = 0;
    bool valid = false;
};

// The official converter writes the same container ("u32 length +
// MediaHeaderPb + raw H.264") for every product; only the declared size
// differs. The overloads without a size use the Turris 1280x720 target.
WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    const std::function<bool()> &isCancelled);
WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    quint32 width, quint32 height,
    const std::function<bool()> &isCancelled);

FrameCountProbeResult parseFrameCountProbe(
    const QByteArray &output, quint32 kind);
FrameCountProbeResult parseFrameCountProbe(
    const QByteArray &output, quint32 kind,
    quint32 width, quint32 height);

bool validateBlob(QFile *file, qint64 declaredSize,
                  const QString &remoteFileName,
                  QString *errorMessage);
bool validateBlob(QFile *file, qint64 declaredSize,
                  const QString &remoteFileName,
                  quint32 width, quint32 height,
                  QString *errorMessage);

// Size of the "u32 length + MediaHeaderPb" prefix when the bytes start with
// a well-formed media header of any size, followed by an Annex-B start
// code; 0 for headerless raw H.264 or anything else. Used to hand only the
// raw stream to tools outside the device path.
qint64 mediaHeaderPrefixSize(const QByteArray &leadingBytes);
inline constexpr qint64 kMaximumMediaHeaderPrefixBytes = 4 + 4096 + 5;

}  // namespace tryx::turris_media
