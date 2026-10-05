#include "privateruntimepaths.h"

#include "printermediafileintegrity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

bool privateDirectoryStatIsValid(const struct stat &status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == S_IRWXU;
}

}  // namespace

namespace tryx::private_runtime_paths {

QString cleanAbsolutePath(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool pathIsInside(const QString &path, const QString &directory) {
    if (path.isEmpty() || directory.isEmpty()) {
        return false;
    }
    const QString cleanPath = cleanAbsolutePath(path);
    const QString cleanDirectory = cleanAbsolutePath(directory);
    if (cleanPath == cleanDirectory) {
        return true;
    }
    const QString directoryPrefix = cleanDirectory.endsWith(QLatin1Char('/'))
        ? cleanDirectory
        : cleanDirectory + QLatin1Char('/');
    return cleanPath.startsWith(directoryPrefix);
}

bool stagedSourceStatIsValid(const struct stat &status) {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1 && status.st_size > 0 &&
           status.st_size <=
               printer_media_file_integrity::kMaximumSourceMediaBytes;
}

bool ensurePrivateDirectory(
    const QString &path, bool create, QString *errorMessage) {
    if (path.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("The private runtime directory path is empty");
        }
        return false;
    }

    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0) {
        if (errno != ENOENT || !create) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Cannot inspect private runtime directory %1: %2")
                    .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
            }
            return false;
        }
        if (::mkdir(encoded.constData(), S_IRWXU) != 0 &&
            errno != EEXIST) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Cannot create private runtime directory %1: %2")
                    .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
            }
            return false;
        }
        if (::lstat(encoded.constData(), &status) != 0) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Cannot verify private runtime directory %1: %2")
                    .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
            }
            return false;
        }
    }
    if (!privateDirectoryStatIsValid(status)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Private runtime directory %1 must be a direct owner-only 0700 directory")
                .arg(path);
        }
        return false;
    }
    return true;
}

bool stagedSourceFileNameIsValid(const QString &fileName) {
    const QFileInfo info(fileName);
    if (info.fileName() != fileName ||
        info.completeBaseName().isEmpty() || info.suffix().isEmpty() ||
        info.suffix() != info.suffix().toLower()) {
        return false;
    }
    static const QSet<QString> supportedSuffixes{
        QStringLiteral("mp4"),  QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("avi"),
        QStringLiteral("mov"),  QStringLiteral("gif"),
        QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("bmp"),
        QStringLiteral("webp"),
    };
    if (!supportedSuffixes.contains(info.suffix())) {
        return false;
    }
    const QUuid parsed(info.completeBaseName());
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces) ==
               info.completeBaseName();
}

bool atomicRenameNoReplace(
    const QString &sourcePath, const QString &destinationPath,
    QString *errorMessage) {
    const QByteArray source = QFile::encodeName(sourcePath);
    const QByteArray destination = QFile::encodeName(destinationPath);
    if (::syscall(SYS_renameat2, AT_FDCWD, source.constData(),
                  AT_FDCWD, destination.constData(),
                  RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = errno == EXDEV
            ? QObject::tr(
                  "The staged source and daemon spool are not on the same filesystem")
            : QObject::tr("Cannot claim staged media source: %1")
                  .arg(QString::fromLocal8Bit(std::strerror(errno)));
    }
    return false;
}

}  // namespace tryx::private_runtime_paths
