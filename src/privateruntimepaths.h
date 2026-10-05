#pragma once

#include <QString>

struct stat;

namespace tryx::private_runtime_paths {

QString cleanAbsolutePath(const QString &path);
bool pathIsInside(const QString &path, const QString &directory);
bool stagedSourceStatIsValid(const struct stat &status);
bool ensurePrivateDirectory(
    const QString &path, bool create, QString *errorMessage);
bool stagedSourceFileNameIsValid(const QString &fileName);
bool atomicRenameNoReplace(
    const QString &sourcePath, const QString &destinationPath,
    QString *errorMessage);

}  // namespace tryx::private_runtime_paths
