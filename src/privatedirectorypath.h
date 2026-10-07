#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace tryx {

// QDir::mkpath() creates directories with 0777 minus the process umask. With
// the 0002 umask many desktop users have, runtime data and cache directories
// become group-writable, and the stores' own safety checks then reject them
// on the next start. This creates every missing component owner-only (0700)
// and leaves existing components as they are. Like mkpath(), it returns true
// when the path is a directory afterwards.
inline bool makePrivateDirectoryPath(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString cleanPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString current;
    const QStringList components = cleanPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        current += QLatin1Char('/') + component;
        const QByteArray encoded = QFile::encodeName(current);
        struct stat status {};
        if (::stat(encoded.constData(), &status) == 0) {
            if (!S_ISDIR(status.st_mode)) {
                return false;
            }
            continue;
        }
        if (errno != ENOENT) {
            return false;
        }
        if (::mkdir(encoded.constData(), S_IRWXU) != 0 && errno != EEXIST) {
            return false;
        }
        if (::stat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode)) {
            return false;
        }
    }
    return true;
}

// Explains why an existing directory fails the runtime's private-directory
// checks, with the command that fixes the common case. Empty when the path is
// a directory of this user that others cannot write, or cannot be inspected.
inline QString privateDirectoryProblem(const QString &path) {
    struct stat status {};
    if (path.isEmpty() || ::lstat(QFile::encodeName(path).constData(), &status) != 0) {
        return {};
    }
    if (S_ISLNK(status.st_mode)) {
        return QStringLiteral("%1 is a symbolic link; it must be a real directory")
            .arg(path);
    }
    if (!S_ISDIR(status.st_mode)) {
        return QStringLiteral("%1 is not a directory").arg(path);
    }
    if (status.st_uid != ::geteuid()) {
        return QStringLiteral("%1 is owned by uid %2, but the TRYX runtime runs as uid %3")
            .arg(path)
            .arg(status.st_uid)
            .arg(::geteuid());
    }
    if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        QString quoted = path;
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QStringLiteral(
                   "%1 has mode %2, so other users can write to it; "
                   "make it private with: chmod go-w '%3'")
            .arg(path)
            .arg(status.st_mode & 07777, 4, 8, QLatin1Char('0'))
            .arg(quoted);
    }
    return {};
}

}  // namespace tryx
