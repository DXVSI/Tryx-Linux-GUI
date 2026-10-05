#include "configurationformatbackup.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
#ifdef TRYX_CONFIGURATION_FORMAT_BACKUP_TESTING
thread_local bool failDirectorySyncForTesting = false;
#endif
bool syncDirectory(int descriptor) {
#ifdef TRYX_CONFIGURATION_FORMAT_BACKUP_TESTING
    if (failDirectorySyncForTesting) return false;
#endif
    return ::fsync(descriptor) == 0;
}

class Descriptor final {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return value_; }
private:
    int value_;
};

bool readFileAt(int directory, const QByteArray &name, qint64 limit, QByteArray *bytes,
                bool privateOnly = false) {
    Descriptor fd(::openat(directory, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat status {};
    if (fd.get() < 0 || ::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_uid != ::geteuid() || status.st_nlink != 1
        || status.st_size <= 0 || status.st_size > limit
        || (privateOnly && (status.st_mode & 0777) != 0600)) return false;
    QFile file;
    if (!file.open(fd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return false;
    *bytes = file.read(limit + 1);
    return file.error() == QFileDevice::NoError && bytes->size() <= limit && file.atEnd()
        && (!privateOnly || ::fsync(fd.get()) == 0);
}
}

#ifdef TRYX_CONFIGURATION_FORMAT_BACKUP_TESTING
void tryx::setConfigurationBackupDirectorySyncFailureForTesting(bool fail) {
    failDirectorySyncForTesting = fail;
}
#endif

bool tryx::preserveConfigurationBeforeUpgrade(const QString &path, qint64 maximumBytes,
                                              int targetVersion, const QList<int> &legacyVersions,
                                              QString *error) {
    const QFileInfo info(path);
    Descriptor directory(info.isAbsolute()
        ? ::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        : -1);
    return preserveConfigurationBeforeUpgradeAt(directory.get(), info.fileName(), maximumBytes, targetVersion, legacyVersions, error);
}

bool tryx::configurationVersionIsSupported(const QString &path, qint64 maximumBytes,
                                           const QList<int> &versions) {
    const QFileInfo info(path);
    if (!info.isAbsolute() || maximumBytes <= 0 || maximumBytes > 16 * 1024 * 1024) return false;
    Descriptor directory(::open(QFile::encodeName(info.absolutePath()).constData(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) return errno == ENOENT;
    struct stat parent {}, before {}, after {};
    if (::fstat(directory.get(), &parent) != 0 || parent.st_uid != ::geteuid()
        || (parent.st_mode & 0022) != 0) return false;
    const QByteArray name = QFile::encodeName(info.fileName());
    if (::fstatat(directory.get(), name.constData(), &before, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    if ((before.st_mode & 07777) != 0600) return false;
    QByteArray bytes;
    if (!readFileAt(directory.get(), name, maximumBytes, &bytes)
        || ::fstatat(directory.get(), name.constData(), &after, AT_SYMLINK_NOFOLLOW) != 0
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_size != after.st_size || before.st_mode != after.st_mode
        || before.st_uid != after.st_uid || before.st_nlink != after.st_nlink
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return false;
    const auto document = QJsonDocument::fromJson(bytes);
    const auto version = document.object().value(QStringLiteral("version"));
    return document.isObject() && version == QJsonValue(version.toInt(-1))
        && versions.contains(version.toInt(-1));
}

bool tryx::preserveConfigurationBeforeUpgradeAt(int directoryDescriptor, const QString &fileName,
    qint64 maximumBytes, int targetVersion, const QList<int> &legacyVersions, QString *error) {
    const auto fail = [error]() {
        if (error) *error = QStringLiteral("Cannot safely preserve the previous configuration format; no upgrade was written.");
        return false;
    };
    if (error) error->clear();
    if (maximumBytes <= 0 || maximumBytes > 16 * 1024 * 1024 || fileName.isEmpty()
        || fileName == QStringLiteral(".") || fileName == QStringLiteral("..")
        || fileName.contains(QLatin1Char('/')) || fileName.contains(QChar(0))) return fail();
    Descriptor directory(::fcntl(directoryDescriptor, F_DUPFD_CLOEXEC, 3));
    struct stat status {};
    if (directory.get() < 0 || ::fstat(directory.get(), &status) != 0
        || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()
        || (status.st_mode & 0022) != 0) return fail();
    const QByteArray name = QFile::encodeName(fileName);
    if (::fstatat(directory.get(), name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? true : fail();
    QByteArray bytes;
    if (!readFileAt(directory.get(), name, maximumBytes, &bytes)) return fail();
    const auto document = QJsonDocument::fromJson(bytes);
    const auto versionValue = document.object().value(QStringLiteral("version"));
    const int version = versionValue.toInt(-1);
    if (!document.isObject() || versionValue != QJsonValue(version)) return fail();
    if (version == targetVersion) return true;
    if (!legacyVersions.contains(version)) return fail();
    const QByteArray backup = name + ".pre-c16-" + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const auto existingBackupIsValid = [&]() {
        QByteArray existing;
        return readFileAt(directory.get(), backup, maximumBytes, &existing, true) && existing == bytes;
    };
    if (::fstatat(directory.get(), backup.constData(), &status, AT_SYMLINK_NOFOLLOW) == 0)
        return existingBackupIsValid() && syncDirectory(directory.get()) ? true : fail();
    if (errno != ENOENT) return fail();
    const QByteArray temporary = backup + ".tmp-" + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    Descriptor output(::openat(directory.get(), temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.get() < 0) return fail();
    QFile file;
    bool ok = file.open(output.get(), QIODevice::WriteOnly, QFileDevice::DontCloseHandle)
        && ::fchmod(output.get(), 0600) == 0 && file.write(bytes) == bytes.size()
        && file.flush() && ::fsync(output.get()) == 0;
    if (ok) {
        // Same no-replace primitive as private_runtime_paths, anchored to the
        // checked directory descriptor so a renamed parent cannot redirect it.
        ok = ::syscall(SYS_renameat2, directory.get(), temporary.constData(),
                       directory.get(), backup.constData(), RENAME_NOREPLACE) == 0;
        if (!ok && errno == EEXIST) ok = existingBackupIsValid();
    }
    ::unlinkat(directory.get(), temporary.constData(), 0); // Only our O_EXCL-created staging name.
    if (ok) ok = syncDirectory(directory.get());
    return ok ? true : fail();
}
