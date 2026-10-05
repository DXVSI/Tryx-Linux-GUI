#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>

namespace tryx::printer_media_file_integrity {

inline constexpr qint64 kMaximumPreparedMediaBytes =
    500LL * 1024LL * 1024LL;
inline constexpr qint64 kMaximumSourceMediaBytes =
    8LL * 1024LL * 1024LL * 1024LL;
inline constexpr qint64 kMaximumThumbnailBytes =
    16LL * 1024LL * 1024LL;

struct SafeSourceHashResult {
    QString sha256;
    qint64 size = 0;
    QString error;
    bool cancelled = false;
};

struct SafePrivateFileHashResult {
    QString sha256;
    qint64 size = 0;
    quint64 device = 0;
    quint64 inode = 0;
    QString error;
    bool cancelled = false;
};

QString sha256File(
    const QString &path,
    const std::function<bool()> &isCancelled = {});
SafeSourceHashResult hashRegularSourceFile(
    const QString &path,
    const std::function<bool()> &isCancelled);
SafePrivateFileHashResult hashPrivateRegularFile(
    const QString &path, qint64 maximumSize,
    const std::function<bool()> &isCancelled = {});
bool isSha256Hex(const QString &value);
QString sourceFingerprint(const QString &path);

}  // namespace tryx::printer_media_file_integrity
