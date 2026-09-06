#pragma once

#include <QList>
#include <QString>

namespace tryx {
// Read-only bounded format gate. Missing files are compatible; unsafe,
// malformed, or future versions are not. Does not migrate or delete anything.
bool configurationVersionIsSupported(const QString &path, qint64 maximumBytes,
                                     const QList<int> &versions);
// Before upgrading a bounded JSON store, preserve the exact old bytes in an
// owner-only, content-addressed sibling. Does not rewrite or downgrade a store.
// Refuses unknown formats, unsafe paths, and an existing mismatched backup.
bool preserveConfigurationBeforeUpgrade(const QString &path, qint64 maximumBytes,
                                        int targetVersion, const QList<int> &legacyVersions,
                                        QString *error);
// Keeps backup and write in the same already-opened directory across renames.
bool preserveConfigurationBeforeUpgradeAt(int directoryDescriptor, const QString &name,
    qint64 maximumBytes, int targetVersion, const QList<int> &legacyVersions, QString *error);
#ifdef TRYX_CONFIGURATION_FORMAT_BACKUP_TESTING
void setConfigurationBackupDirectorySyncFailureForTesting(bool fail);
#endif
}
