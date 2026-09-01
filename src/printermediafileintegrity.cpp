#include "printermediafileintegrity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool sameFileTimestamp(const timespec &left, const timespec &right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

}  // namespace

namespace tryx::printer_media_file_integrity {

QString sha256File(
    const QString &path,
    const std::function<bool()> &isCancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (isCancelled && isCancelled()) {
            return {};
        }
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(chunk);
    }
    if (isCancelled && isCancelled()) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

SafeSourceHashResult hashRegularSourceFile(
    const QString &path,
    const std::function<bool()> &isCancelled) {
    SafeSourceHashResult result;
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        return result;
    }
    const QByteArray encodedPath = QFile::encodeName(path);
    const int descriptor = ::open(
        encodedPath.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        result.error = QObject::tr("Cannot open source media safely: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return result;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        result.error = QObject::tr("Cannot read source media: %1")
                           .arg(file.errorString());
        return result;
    }
    struct stat before {};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 ||
        before.st_size > kMaximumSourceMediaBytes) {
        result.error = QObject::tr(
            "Source media is not a bounded regular file");
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (isCancelled && isCancelled()) {
            result.cancelled = true;
            return result;
        }
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            result.error = QObject::tr("Cannot hash source media: %1")
                               .arg(file.errorString());
            return result;
        }
        hash.addData(chunk);
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        !sameFileTimestamp(before.st_mtim, after.st_mtim) ||
        !sameFileTimestamp(before.st_ctim, after.st_ctim)) {
        result.error = QObject::tr(
            "Source media changed while its content hash was calculated");
        return result;
    }
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        return result;
    }
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    result.size = static_cast<qint64>(after.st_size);
    return result;
}

SafePrivateFileHashResult hashPrivateRegularFile(
    const QString &path, qint64 maximumSize,
    const std::function<bool()> &isCancelled) {
    SafePrivateFileHashResult result;
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        return result;
    }
    if (maximumSize <= 0 ||
        maximumSize > kMaximumPreparedMediaBytes) {
        result.error = QObject::tr(
            "Retry-cache artifact size limit is invalid");
        return result;
    }

    const QByteArray encodedPath = QFile::encodeName(path);
    const int descriptor = ::open(
        encodedPath.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        result.error = QObject::tr(
                           "Cannot open retry-cache artifact safely: %1")
                           .arg(QString::fromLocal8Bit(
                               std::strerror(errno)));
        return result;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        result.error = QObject::tr(
                           "Cannot read retry-cache artifact: %1")
                           .arg(file.errorString());
        return result;
    }

    struct stat before {};
    constexpr mode_t forbiddenModeBits =
        S_IWGRP | S_IWOTH | S_IXUSR | S_IXGRP | S_IXOTH |
        S_ISUID | S_ISGID | S_ISVTX;
    if (::fstat(descriptor, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        (before.st_mode & (S_IRUSR | S_IWUSR)) !=
            (S_IRUSR | S_IWUSR) ||
        (before.st_mode & forbiddenModeBits) != 0 ||
        before.st_nlink < 1 || before.st_nlink > 3 ||
        before.st_size <= 0 || before.st_size > maximumSize) {
        result.error = QObject::tr(
            "Retry-cache artifact is not a bounded safe regular file");
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (isCancelled && isCancelled()) {
            result.cancelled = true;
            return result;
        }
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            result.error = QObject::tr(
                               "Cannot hash retry-cache artifact: %1")
                               .arg(file.errorString());
            return result;
        }
        hash.addData(chunk);
    }

    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mode != after.st_mode ||
        before.st_uid != after.st_uid ||
        before.st_nlink != after.st_nlink ||
        !sameFileTimestamp(before.st_mtim, after.st_mtim) ||
        !sameFileTimestamp(before.st_ctim, after.st_ctim)) {
        result.error = QObject::tr(
            "Retry-cache artifact changed while its content hash was calculated");
        return result;
    }
    if (isCancelled && isCancelled()) {
        result.cancelled = true;
        return result;
    }

    result.sha256 = QString::fromLatin1(hash.result().toHex());
    result.size = static_cast<qint64>(after.st_size);
    result.device = static_cast<quint64>(after.st_dev);
    result.inode = static_cast<quint64>(after.st_ino);
    return result;
}

bool isSha256Hex(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const bool decimal =
            character >= QLatin1Char('0') &&
            character <= QLatin1Char('9');
        const bool lowercaseHex =
            character >= QLatin1Char('a') &&
            character <= QLatin1Char('f');
        if (!decimal && !lowercaseHex) {
            return false;
        }
    }
    return true;
}

QString sourceFingerprint(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return {};
    }
    const QByteArray identity =
        info.canonicalFilePath().toUtf8() + '\0' +
        QByteArray::number(info.size()) + '\0' +
        QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    return QString::fromLatin1(
        QCryptographicHash::hash(
            identity, QCryptographicHash::Sha256).toHex());
}

}  // namespace tryx::printer_media_file_integrity
