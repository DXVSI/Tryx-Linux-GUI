#pragma once

#include <QString>
#include <QByteArray>
#include <QtGlobal>

#include <functional>

class QFile;

namespace tryx::turris_media {

inline constexpr quint16 kProductId = 0x2011;
// MediaHeaderPb.original_media_type (Tryx.Media.enOriginalMediaType in
// KANALI 2.6.0 MediaX.dll): JPEG=1, PNG=2, GIF=3, MP4=4. This application
// declares every still it writes as PNG; a JPEG still from the official
// application is still recognised when it is read back.
inline constexpr quint32 kJpegImageKind = 1U;
inline constexpr quint32 kImageKind = 2U;
inline constexpr quint32 kGifKind = 3U;
// The official converter overlays a still on its background with
// shortest=1, so a still image is exactly one encoded frame.
inline constexpr quint64 kStillImageFrames = 1ULL;
inline constexpr quint32 kVideoKind = 4U;
inline constexpr quint32 kWidth = 1280U;
inline constexpr quint32 kHeight = 720U;

// A JPEG or PNG still: one frame at 30 fps.
bool isStillImageKind(quint32 kind);

// Frame rate the official application encodes and declares per media kind:
// stills at 30 fps, video and animated GIF at 60 fps.
quint32 framesPerSecondForKind(quint32 kind);
// The same for media made for one area of a split screen: the official
// application encodes split-area (1:1) video and GIF at 30 fps as well.
quint32 framesPerSecondForKind(quint32 kind, bool splitArea);

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
// differs. The overloads without a size use the Turris 1280x720 target, and
// those without splitArea declare the full-screen frame rate of the kind.
WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    const std::function<bool()> &isCancelled);
WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    quint32 width, quint32 height,
    const std::function<bool()> &isCancelled);
WriteResult writeBlob(
    const QString &rawPath, const QString &outputPath,
    quint32 kind, quint64 frameCount,
    quint32 width, quint32 height, bool splitArea,
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
bool validateBlob(QFile *file, qint64 declaredSize,
                  const QString &remoteFileName,
                  quint32 width, quint32 height, bool splitArea,
                  QString *errorMessage);

// A well-formed media header at the start of a file, followed by an
// Annex-B start code. size is the length of the "u32 length +
// MediaHeaderPb" prefix, 0 for headerless raw H.264 or anything else.
struct MediaHeaderPrefix {
    qint64 size = 0;
    quint32 kind = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint64 frameCount = 0;
};
inline constexpr qint64 kMaximumMediaHeaderPrefixBytes = 4 + 4096 + 5;
MediaHeaderPrefix parseMediaHeaderPrefix(const QByteArray &leadingBytes);
// Reads the first kMaximumMediaHeaderPrefixBytes of a file.
MediaHeaderPrefix readMediaHeaderPrefix(const QString &path);
// Size of the prefix, used to hand only the raw stream to tools outside the
// device path.
qint64 mediaHeaderPrefixSize(const QByteArray &leadingBytes);
// Whether a file pulled from the device is a still: it starts with a media
// header that declares a JPEG or PNG original.
bool isStillImageMediaHeader(const MediaHeaderPrefix &header);

}  // namespace tryx::turris_media
