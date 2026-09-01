#include "guiautostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringList>

#include <atomic>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace gui_autostart {

namespace {

class AutostartTranslations final {
    Q_DECLARE_TR_FUNCTIONS(AppSettingsController)
};

constexpr qint64 kMaximumDesktopEntryBytes = 64 * 1024;
const QString kAutostartDirectoryName =
    QStringLiteral("autostart");
const QString kAutostartEntryName =
    QStringLiteral("tryx-panorama-manager.desktop");
const QString kManagedMarker =
    QStringLiteral("X-TRYX-Panorama-Managed");
thread_local bool gUserConfigDirectoryOverrideSet = false;
thread_local QString gUserConfigDirectoryOverride;
thread_local testing::BeforeUserEntryOpenHook
    gBeforeUserEntryOpenHook = nullptr;
thread_local testing::BeforeLeafNamespaceMutationHook
    gBeforeLeafNamespaceMutationHook = nullptr;
thread_local testing::AfterLeafPreconditionCheckHook
    gAfterLeafPreconditionCheckHook = nullptr;
thread_local testing::AfterLeafNamespaceMutationHook
    gAfterLeafNamespaceMutationHook = nullptr;
std::atomic<quint64> gTemporaryEntrySequence{0};

struct FileIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

struct UserEntry {
    bool exists = false;
    QByteArray contents;
    FileIdentity identity;
    QString error;
};

struct DesktopEntry {
    bool managed = false;
    bool hidden = false;
    bool hasOnlyShowIn = false;
    bool hasNotShowIn = false;
    QString exec;
    QString tryExec;
    QStringList onlyShowIn;
    QStringList notShowIn;
    QString error;
};

struct SystemEntry {
    bool exists = false;
    bool enabled = false;
    QString error;
};

struct AutostartState {
    bool available = false;
    bool enabled = false;
    QString error;
};

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
    ScopedFileDescriptor &operator=(
        const ScopedFileDescriptor &) = delete;

    ScopedFileDescriptor(ScopedFileDescriptor &&other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }

    ScopedFileDescriptor &operator=(
        ScopedFileDescriptor &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = other.descriptor_;
            other.descriptor_ = -1;
        }
        return *this;
    }

    int get() const {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

QString userConfigDirectory() {
    if (gUserConfigDirectoryOverrideSet) {
        return gUserConfigDirectoryOverride;
    }
    return QStandardPaths::writableLocation(
        QStandardPaths::GenericConfigLocation);
}

QString autostartDirectoryPath() {
    const QString configDirectory = userConfigDirectory();
    if (configDirectory.isEmpty()) {
        return {};
    }
    return QDir(configDirectory).filePath(
        kAutostartDirectoryName);
}

QString autostartEntryPath(const QString &configDirectory) {
    if (configDirectory.isEmpty()) {
        return {};
    }
    return QDir(configDirectory).filePath(
        kAutostartDirectoryName + QLatin1Char('/') +
        kAutostartEntryName);
}

QString userAutostartEntryPath() {
    return autostartEntryPath(userConfigDirectory());
}

QString pathError(const QString &message,
                  const QString &path) {
    return AutostartTranslations::tr("%1: %2")
        .arg(message, path);
}

bool userConfigStatusIsSafe(const struct stat &status,
                            bool exactOwnerOnly,
                            const QString &path,
                            QString *error) {
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration path is not a directory"),
            path);
        return false;
    }
    if (status.st_uid != ::getuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (exactOwnerOnly &&
         (status.st_mode & 0777) != S_IRWXU)) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration directory is unavailable"),
            path);
        return false;
    }
    return true;
}

bool userConfigParentStatusIsSafe(const struct stat &status,
                                  const QString &path,
                                  QString *error) {
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration path is not a directory"),
            path);
        return false;
    }
    if ((status.st_uid != ::getuid() && status.st_uid != 0) ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration directory is unavailable"),
            path);
        return false;
    }
    return true;
}

bool inspectUserConfigDirectory(bool createIfMissing,
                                bool *missing,
                                QString *error) {
    *missing = false;
    const QString configuredPath = userConfigDirectory();
    if (configuredPath.isEmpty() ||
        !QDir::isAbsolutePath(configuredPath)) {
        *error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return false;
    }

    const QString configDirectory = QDir::cleanPath(configuredPath);
    const QFileInfo configInfo(configDirectory);
    const QString parentPath = configInfo.absolutePath();
    const QByteArray leafName = QFile::encodeName(configInfo.fileName());
    if (leafName.isEmpty() || leafName == QByteArrayLiteral(".") ||
        leafName == QByteArrayLiteral("..") ||
        parentPath == configDirectory) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration directory is unavailable"),
            configDirectory);
        return false;
    }

    const QByteArray encodedParent = QFile::encodeName(parentPath);
    ScopedFileDescriptor parent(::open(
        encodedParent.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent.get() < 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration directory is unavailable"),
            parentPath);
        return false;
    }

    struct stat parentStatus {};
    if (::fstat(parent.get(), &parentStatus) != 0 ||
        !userConfigParentStatusIsSafe(
            parentStatus, parentPath, error)) {
        if (error->isEmpty()) {
            *error = pathError(
                AutostartTranslations::tr(
                    "The user configuration directory is unavailable"),
                parentPath);
        }
        return false;
    }

    struct stat configStatus {};
    if (::fstatat(parent.get(), leafName.constData(), &configStatus,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        return userConfigStatusIsSafe(
            configStatus, false, configDirectory, error);
    }
    if (errno != ENOENT) {
        *error = pathError(
            AutostartTranslations::tr(
                "The user configuration directory is unavailable"),
            configDirectory);
        return false;
    }

    *missing = true;
    if (!createIfMissing) {
        return true;
    }
    if (::mkdirat(parent.get(), leafName.constData(), S_IRWXU) != 0) {
        if (errno != EEXIST ||
            ::fstatat(parent.get(), leafName.constData(), &configStatus,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !userConfigStatusIsSafe(
                configStatus, false, configDirectory, error)) {
            if (error->isEmpty()) {
                *error = pathError(
                    AutostartTranslations::tr(
                        "Failed to create the user configuration directory"),
                    configDirectory);
            }
            return false;
        }
        *missing = false;
        return true;
    }

    const auto discardCreatedDirectory = [&]() {
        ::unlinkat(parent.get(), leafName.constData(), AT_REMOVEDIR);
    };
    if (::fchmodat(parent.get(), leafName.constData(), S_IRWXU,
                   AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstatat(parent.get(), leafName.constData(), &configStatus,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !userConfigStatusIsSafe(
            configStatus, true, configDirectory, error)) {
        discardCreatedDirectory();
        if (error->isEmpty()) {
            *error = pathError(
                AutostartTranslations::tr(
                    "Failed to create the user configuration directory"),
                configDirectory);
        }
        return false;
    }
    *missing = false;
    return true;
}

bool inspectOwnedDirectory(const QString &path,
                           bool *missing,
                           QString *error) {
    *missing = false;
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0) {
        if (errno == ENOENT) {
            *missing = true;
            return true;
        }
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to inspect the autostart directory"),
            path);
        return false;
    }
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        *error = pathError(
            AutostartTranslations::tr(
                "The autostart path is not a real directory"),
            path);
        return false;
    }
    if (status.st_uid != ::getuid()) {
        *error = pathError(
            AutostartTranslations::tr(
                "The autostart directory is not owned by the current user"),
            path);
        return false;
    }
    if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "The autostart directory permissions are unsafe"),
            path);
        return false;
    }
    return true;
}

bool ensureAutostartDirectory(QString *error) {
    const QString configDirectory = userConfigDirectory();
    bool configMissing = false;
    if (!inspectUserConfigDirectory(
            true, &configMissing, error) || configMissing) {
        return false;
    }

    const QString directory = autostartDirectoryPath();
    const QByteArray encodedConfig =
        QFile::encodeName(QDir::cleanPath(configDirectory));
    ScopedFileDescriptor configDescriptor(::open(
        encodedConfig.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat configStatus {};
    if (configDescriptor.get() < 0 ||
        ::fstat(configDescriptor.get(), &configStatus) != 0 ||
        !userConfigStatusIsSafe(
            configStatus, false, configDirectory, error)) {
        if (error->isEmpty()) {
            *error = pathError(
                AutostartTranslations::tr(
                    "The user configuration directory is unavailable"),
                configDirectory);
        }
        return false;
    }

    const QByteArray leafName =
        kAutostartDirectoryName.toUtf8();
    struct stat directoryStatus {};
    if (::fstatat(
            configDescriptor.get(), leafName.constData(),
            &directoryStatus, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISDIR(directoryStatus.st_mode) ||
            directoryStatus.st_uid != ::getuid() ||
            (directoryStatus.st_mode &
             (S_IWGRP | S_IWOTH)) != 0) {
            *error = pathError(
                AutostartTranslations::tr(
                    "The autostart directory permissions are unsafe"),
                directory);
            return false;
        }
        return true;
    }
    if (errno != ENOENT ||
        ::mkdirat(
            configDescriptor.get(), leafName.constData(),
            S_IRWXU) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to create the autostart directory"),
            directory);
        return false;
    }

    const auto discardCreatedDirectory = [&]() {
        ::unlinkat(
            configDescriptor.get(), leafName.constData(),
            AT_REMOVEDIR);
    };
    if (::fchmodat(
            configDescriptor.get(), leafName.constData(),
            S_IRWXU, AT_SYMLINK_NOFOLLOW) != 0 ||
        ::fstatat(
            configDescriptor.get(), leafName.constData(),
            &directoryStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(directoryStatus.st_mode) ||
        directoryStatus.st_uid != ::getuid() ||
        (directoryStatus.st_mode & 0777) != S_IRWXU) {
        discardCreatedDirectory();
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to secure the new autostart directory"),
            directory);
        return false;
    }
    return true;
}

bool sameIdentity(const FileIdentity &left,
                  const FileIdentity &right) {
    return left.device == right.device &&
           left.inode == right.inode;
}

UserEntry inspectUserEntryAt(int directoryDescriptor,
                             const QByteArray &leafName,
                             const QString &path,
                             bool invokeBeforeOpenHook) {
    UserEntry result;
    if (directoryDescriptor < 0 || path.isEmpty()) {
        result.error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return result;
    }
    struct stat pathStatus {};
    if (::fstatat(
            directoryDescriptor, leafName.constData(),
            &pathStatus, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return result;
        }
        result.error = pathError(
            AutostartTranslations::tr(
                "Failed to inspect the GUI autostart entry"),
            path);
        return result;
    }
    result.exists = true;
    if (!S_ISREG(pathStatus.st_mode) ||
        pathStatus.st_uid != ::getuid() ||
        pathStatus.st_nlink != 1 ||
        (pathStatus.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        pathStatus.st_size < 0 ||
        pathStatus.st_size > kMaximumDesktopEntryBytes) {
        result.error = pathError(
            AutostartTranslations::tr(
                "The GUI autostart entry is unsafe"),
            path);
        return result;
    }

    if (invokeBeforeOpenHook && gBeforeUserEntryOpenHook) {
        gBeforeUserEntryOpenHook(path);
    }

    const int descriptor = ::openat(
        directoryDescriptor, leafName.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        result.error = pathError(
            AutostartTranslations::tr(
                "Failed to open the GUI autostart entry safely"),
            path);
        return result;
    }

    struct stat openedStatus {};
    if (::fstat(descriptor, &openedStatus) != 0 ||
        !S_ISREG(openedStatus.st_mode) ||
        openedStatus.st_uid != ::getuid() ||
        openedStatus.st_nlink != 1 ||
        (openedStatus.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        openedStatus.st_size < 0 ||
        openedStatus.st_size > kMaximumDesktopEntryBytes ||
        openedStatus.st_dev != pathStatus.st_dev ||
        openedStatus.st_ino != pathStatus.st_ino) {
        ::close(descriptor);
        result.error = pathError(
            AutostartTranslations::tr(
                "The GUI autostart entry changed while it was inspected"),
            path);
        return result;
    }

    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(openedStatus.st_size));
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(
            descriptor, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(descriptor);
            result.error = pathError(
                AutostartTranslations::tr(
                    "Failed to read the GUI autostart entry"),
                path);
            return result;
        }
        if (contents.size() + count >
            kMaximumDesktopEntryBytes) {
            ::close(descriptor);
            result.error = pathError(
                AutostartTranslations::tr(
                    "The GUI autostart entry is too large"),
                path);
            return result;
        }
        contents.append(buffer, static_cast<qsizetype>(count));
    }
    ::close(descriptor);

    result.contents = contents;
    result.identity.device = openedStatus.st_dev;
    result.identity.inode = openedStatus.st_ino;
    return result;
}

UserEntry inspectUserEntry() {
    const QString directoryPath = autostartDirectoryPath();
    const QString entryPath = userAutostartEntryPath();
    if (directoryPath.isEmpty() || entryPath.isEmpty()) {
        UserEntry result;
        result.error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return result;
    }
    const QByteArray encodedDirectory =
        QFile::encodeName(directoryPath);
    ScopedFileDescriptor directoryDescriptor(::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        UserEntry result;
        if (errno != ENOENT) {
            result.error = pathError(
                AutostartTranslations::tr(
                    "Failed to inspect the autostart directory"),
                directoryPath);
        }
        return result;
    }
    return inspectUserEntryAt(
        directoryDescriptor.get(),
        kAutostartEntryName.toUtf8(), entryPath, true);
}

bool decodeDesktopString(const QString &encoded,
                         QString *decoded) {
    QString result;
    result.reserve(encoded.size());
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        const QChar character = encoded.at(index);
        if (character != QLatin1Char('\\')) {
            result.append(character);
            continue;
        }
        if (++index >= encoded.size()) {
            return false;
        }
        const QChar escaped = encoded.at(index);
        if (escaped == QLatin1Char('s')) {
            result.append(QLatin1Char(' '));
        } else if (escaped == QLatin1Char('n')) {
            result.append(QLatin1Char('\n'));
        } else if (escaped == QLatin1Char('t')) {
            result.append(QLatin1Char('\t'));
        } else if (escaped == QLatin1Char('r')) {
            result.append(QLatin1Char('\r'));
        } else if (escaped == QLatin1Char('\\')) {
            result.append(QLatin1Char('\\'));
        } else {
            return false;
        }
    }
    *decoded = result;
    return true;
}

bool decodeDesktopStringList(const QString &encoded,
                             QStringList *decoded) {
    QStringList result;
    QString value;
    value.reserve(encoded.size());
    const auto appendValue = [&result, &value]() {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            result.append(trimmed);
        }
        value.clear();
    };
    for (qsizetype index = 0; index < encoded.size(); ++index) {
        const QChar character = encoded.at(index);
        if (character == QLatin1Char(';')) {
            appendValue();
            continue;
        }
        if (character != QLatin1Char('\\')) {
            value.append(character);
            continue;
        }
        if (++index >= encoded.size()) {
            return false;
        }
        const QChar escaped = encoded.at(index);
        if (escaped == QLatin1Char('s')) {
            value.append(QLatin1Char(' '));
        } else if (escaped == QLatin1Char('n')) {
            value.append(QLatin1Char('\n'));
        } else if (escaped == QLatin1Char('t')) {
            value.append(QLatin1Char('\t'));
        } else if (escaped == QLatin1Char('r')) {
            value.append(QLatin1Char('\r'));
        } else if (escaped == QLatin1Char('\\')) {
            value.append(QLatin1Char('\\'));
        } else if (escaped == QLatin1Char(';')) {
            value.append(QLatin1Char(';'));
        } else {
            return false;
        }
    }
    appendValue();
    *decoded = result;
    return true;
}

QString decodedExecForExecutable(const QString &path) {
    QString escaped = QStringLiteral("\"");
    for (const QChar character : path) {
        if (character == QLatin1Char('\\')) {
            escaped.append(QStringLiteral("\\\\"));
        } else if (character == QLatin1Char('"') ||
                   character == QLatin1Char('`') ||
                   character == QLatin1Char('$')) {
            escaped.append(QLatin1Char('\\'));
            escaped.append(character);
        } else if (character == QLatin1Char('%')) {
            escaped.append(QStringLiteral("%%"));
        } else {
            escaped.append(character);
        }
    }
    escaped.append(QStringLiteral("\" --autostart"));
    return escaped;
}

DesktopEntry parseDesktopEntry(const QByteArray &contents,
                               bool requireManaged) {
    DesktopEntry result;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(contents);
    if (decoder.hasError()) {
        result.error = AutostartTranslations::tr(
            "The GUI autostart entry is not valid UTF-8");
        return result;
    }

    QMap<QString, QString> values;
    QMap<QString, QStringList> stringLists;
    bool inDesktopEntry = false;
    bool sawDesktopEntry = false;
    bool hasUnsupportedManagedContent = false;
    int desktopEntrySectionCount = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('#'))) {
            hasUnsupportedManagedContent = true;
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('['))) {
            if (!trimmed.endsWith(QLatin1Char(']'))) {
                result.error = AutostartTranslations::tr(
                    "The GUI autostart entry has a malformed section");
                return result;
            }
            inDesktopEntry =
                trimmed == QStringLiteral("[Desktop Entry]");
            if (inDesktopEntry) {
                ++desktopEntrySectionCount;
            } else {
                hasUnsupportedManagedContent = true;
            }
            sawDesktopEntry = sawDesktopEntry || inDesktopEntry;
            continue;
        }
        if (!inDesktopEntry) {
            hasUnsupportedManagedContent = true;
            continue;
        }
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            result.error = AutostartTranslations::tr(
                "The GUI autostart entry has a malformed key");
            return result;
        }
        const QString key = line.left(separator).trimmed();
        if (key.isEmpty() || values.contains(key)) {
            result.error = AutostartTranslations::tr(
                "The GUI autostart entry has a duplicate or empty key");
            return result;
        }
        const QString encodedValue =
            line.mid(separator + 1).trimmed();
        QString value;
        const bool isStringList =
            key == QStringLiteral("OnlyShowIn") ||
            key == QStringLiteral("NotShowIn");
        QStringList stringList;
        const bool decoded = isStringList
            ? decodeDesktopStringList(encodedValue, &stringList)
            : decodeDesktopString(encodedValue, &value);
        if (!decoded) {
            result.error = AutostartTranslations::tr(
                "The GUI autostart entry has an invalid escape sequence");
            return result;
        }
        if (isStringList) {
            stringLists.insert(key, stringList);
            value = stringList.join(QLatin1Char(';'));
        }
        values.insert(key, value);
    }

    auto requireBoolean = [&values, &result](
                              const QString &key,
                              bool defaultValue,
                              bool required,
                              bool *value) {
        if (!values.contains(key)) {
            if (required) {
                result.error = AutostartTranslations::tr(
                    "The GUI autostart entry is missing %1")
                    .arg(key);
                return false;
            }
            *value = defaultValue;
            return true;
        }
        const QString raw = values.value(key);
        if (raw != QStringLiteral("true") &&
            raw != QStringLiteral("false")) {
            result.error = AutostartTranslations::tr(
                "The GUI autostart entry has an invalid %1 value")
                .arg(key);
            return false;
        }
        *value = raw == QStringLiteral("true");
        return true;
    };

    if (!sawDesktopEntry) {
        result.error = AutostartTranslations::tr(
            "The GUI autostart entry is not a valid application entry");
        return result;
    }
    if (!requireBoolean(
            QStringLiteral("Hidden"), false, false,
            &result.hidden)) {
        return result;
    }
    result.tryExec = values.value(QStringLiteral("TryExec"));
    result.exec = values.value(QStringLiteral("Exec"));
    result.managed =
        values.value(kManagedMarker) == QStringLiteral("true");

    if (!requireManaged && result.hidden) {
        return result;
    }
    if (values.value(QStringLiteral("Type")) !=
            QStringLiteral("Application") ||
        values.value(QStringLiteral("Name")).isEmpty() ||
        result.exec.isEmpty()) {
        result.error = AutostartTranslations::tr(
            "The GUI autostart entry is not a valid application entry");
        return result;
    }
    if (!requireManaged) {
        result.hasOnlyShowIn =
            values.contains(QStringLiteral("OnlyShowIn"));
        result.hasNotShowIn =
            values.contains(QStringLiteral("NotShowIn"));
        if (result.hasOnlyShowIn && result.hasNotShowIn) {
            result.error = AutostartTranslations::tr(
                "The system GUI autostart entry has both OnlyShowIn and NotShowIn");
            return result;
        }
        result.onlyShowIn =
            stringLists.value(QStringLiteral("OnlyShowIn"));
        result.notShowIn =
            stringLists.value(QStringLiteral("NotShowIn"));
        return result;
    }

    const QStringList managedKeys{
        QStringLiteral("Type"),
        QStringLiteral("Name"),
        QStringLiteral("Name[ru]"),
        QStringLiteral("Exec"),
        QStringLiteral("TryExec"),
        QStringLiteral("Icon"),
        QStringLiteral("Terminal"),
        QStringLiteral("StartupNotify"),
        QStringLiteral("Hidden"),
        kManagedMarker};
    bool hasExactManagedKeys =
        values.size() == managedKeys.size() &&
        desktopEntrySectionCount == 1 &&
        !hasUnsupportedManagedContent;
    for (const QString &key : managedKeys) {
        hasExactManagedKeys =
            hasExactManagedKeys && values.contains(key);
    }
    bool terminal = true;
    bool startupNotify = true;
    if (!hasExactManagedKeys ||
        !result.managed ||
        values.value(QStringLiteral("Name")) !=
            QStringLiteral("TRYX Panorama Manager") ||
        values.value(QStringLiteral("Name[ru]")) !=
            QStringLiteral("Менеджер TRYX Panorama") ||
        result.tryExec.isEmpty() ||
        result.exec != decodedExecForExecutable(result.tryExec) ||
        !values.contains(QStringLiteral("Hidden")) ||
        values.value(QStringLiteral("Icon")) !=
            QStringLiteral("tryx-panorama") ||
        !requireBoolean(
            QStringLiteral("Terminal"), false, true,
            &terminal) ||
        !requireBoolean(
            QStringLiteral("StartupNotify"), false, true,
            &startupNotify) ||
        terminal || startupNotify) {
        if (result.error.isEmpty()) {
            result.error = AutostartTranslations::tr(
                "The existing GUI autostart entry is foreign or incompatible");
        }
        return result;
    }
    return result;
}

bool executableExists(const QString &tryExec) {
    if (tryExec.isEmpty()) {
        return true;
    }
    const QFileInfo executable(tryExec);
    if (executable.isAbsolute()) {
        return executable.isFile() && executable.isExecutable();
    }
    if (tryExec.contains(QLatin1Char('/'))) {
        return false;
    }
    return !QStandardPaths::findExecutable(tryExec).isEmpty();
}

bool systemEntryIsVisible(const DesktopEntry &entry) {
    if (entry.hidden) {
        return false;
    }
    QStringList currentDesktops;
    const QStringList encodedDesktops =
        qEnvironmentVariable("XDG_CURRENT_DESKTOP")
            .split(QLatin1Char(':'), Qt::KeepEmptyParts);
    for (const QString &desktop : encodedDesktops) {
        const QString trimmed = desktop.trimmed();
        if (!trimmed.isEmpty()) {
            currentDesktops.append(trimmed);
        }
    }
    const auto matchesCurrentDesktop =
        [&currentDesktops](const QStringList &configured) {
            for (const QString &desktop : currentDesktops) {
                if (configured.contains(desktop)) {
                    return true;
                }
            }
            return false;
        };
    if (entry.hasOnlyShowIn) {
        return matchesCurrentDesktop(entry.onlyShowIn);
    }
    if (entry.hasNotShowIn) {
        return !matchesCurrentDesktop(entry.notShowIn);
    }
    return true;
}

SystemEntry inspectSystemEntry() {
    SystemEntry result;
    const QString userConfig = QDir::cleanPath(userConfigDirectory());
    const QStringList configLocations =
        QStandardPaths::standardLocations(
            QStandardPaths::GenericConfigLocation);
    for (const QString &configLocation : configLocations) {
        if (QDir::cleanPath(configLocation) == userConfig) {
            continue;
        }
        const QString path = autostartEntryPath(configLocation);
        if (path.isEmpty()) {
            continue;
        }
        const QByteArray encoded = QFile::encodeName(path);
        struct stat status {};
        if (::lstat(encoded.constData(), &status) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            result.error = pathError(
                AutostartTranslations::tr(
                    "Failed to inspect the system GUI autostart entry"),
                path);
            return result;
        }
        result.exists = true;
        if (!S_ISREG(status.st_mode) ||
            status.st_size < 0 ||
            status.st_size > kMaximumDesktopEntryBytes) {
            result.error = pathError(
                AutostartTranslations::tr(
                    "The system GUI autostart entry is unsafe"),
                path);
            return result;
        }
        const int descriptor = ::open(
            encoded.constData(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0) {
            result.error = pathError(
                AutostartTranslations::tr(
                    "Failed to open the system GUI autostart entry safely"),
                path);
            return result;
        }

        struct stat openedStatus {};
        if (::fstat(descriptor, &openedStatus) != 0 ||
            !S_ISREG(openedStatus.st_mode) ||
            openedStatus.st_size < 0 ||
            openedStatus.st_size > kMaximumDesktopEntryBytes ||
            openedStatus.st_dev != status.st_dev ||
            openedStatus.st_ino != status.st_ino) {
            ::close(descriptor);
            result.error = pathError(
                AutostartTranslations::tr(
                    "The system GUI autostart entry changed while it was inspected"),
                path);
            return result;
        }

        QByteArray contents;
        contents.reserve(
            static_cast<qsizetype>(openedStatus.st_size));
        char buffer[4096];
        while (true) {
            const ssize_t count = ::read(
                descriptor, buffer, sizeof(buffer));
            if (count == 0) {
                break;
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(descriptor);
                result.error = pathError(
                    AutostartTranslations::tr(
                        "Failed to read the system GUI autostart entry"),
                    path);
                return result;
            }
            if (contents.size() + count >
                kMaximumDesktopEntryBytes) {
                ::close(descriptor);
                result.error = pathError(
                    AutostartTranslations::tr(
                        "The system GUI autostart entry is too large"),
                    path);
                return result;
            }
            contents.append(
                buffer, static_cast<qsizetype>(count));
        }
        ::close(descriptor);
        const DesktopEntry entry =
            parseDesktopEntry(contents, false);
        if (!entry.error.isEmpty()) {
            result.error = entry.error;
            return result;
        }
        result.enabled =
            systemEntryIsVisible(entry) &&
            executableExists(entry.tryExec);
        return result;
    }
    return result;
}

AutostartState queryAutostartState() {
    AutostartState state;
    const QString directory = autostartDirectoryPath();
    if (directory.isEmpty()) {
        state.error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return state;
    }

    bool configMissing = false;
    if (!inspectUserConfigDirectory(
            false, &configMissing, &state.error)) {
        return state;
    }

    bool directoryMissing = false;
    if (!configMissing) {
        if (!inspectOwnedDirectory(
                directory, &directoryMissing, &state.error)) {
            return state;
        }
    } else {
        directoryMissing = true;
    }
    if (!directoryMissing) {
        const UserEntry userEntry = inspectUserEntry();
        if (!userEntry.error.isEmpty()) {
            state.error = userEntry.error;
            return state;
        }
        if (userEntry.exists) {
            const DesktopEntry entry =
                parseDesktopEntry(userEntry.contents, true);
            if (!entry.error.isEmpty()) {
                state.error = entry.error;
                return state;
            }
            state.available = true;
            state.enabled =
                !entry.hidden &&
                entry.tryExec ==
                    QCoreApplication::applicationFilePath() &&
                executableExists(entry.tryExec);
            return state;
        }
    }

    const SystemEntry systemEntry = inspectSystemEntry();
    if (!systemEntry.error.isEmpty()) {
        state.error = systemEntry.error;
        return state;
    }
    state.available = true;
    state.enabled = systemEntry.enabled;
    return state;
}

bool executablePathIsRepresentable(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    for (const QChar character : path) {
        const ushort codePoint = character.unicode();
        if (codePoint <= 0x001f || codePoint == 0x007f ||
            character == QLatin1Char('=')) {
            return false;
        }
    }
    return true;
}

QString escapeDesktopString(const QString &value) {
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar character : value) {
        if (character == QLatin1Char('\\')) {
            escaped.append(QStringLiteral("\\\\"));
        } else if (character == QLatin1Char(' ')) {
            escaped.append(QStringLiteral("\\s"));
        } else if (character == QLatin1Char('\n')) {
            escaped.append(QStringLiteral("\\n"));
        } else if (character == QLatin1Char('\r')) {
            escaped.append(QStringLiteral("\\r"));
        } else if (character == QLatin1Char('\t')) {
            escaped.append(QStringLiteral("\\t"));
        } else {
            escaped.append(character);
        }
    }
    return escaped;
}

QString escapeExecExecutable(const QString &path) {
    QString escaped = QStringLiteral("\"");
    escaped.reserve(path.size() + 2);
    for (const QChar character : path) {
        if (character == QLatin1Char('\\')) {
            escaped.append(QStringLiteral("\\\\\\\\"));
        } else if (character == QLatin1Char('"') ||
                   character == QLatin1Char('`') ||
                   character == QLatin1Char('$')) {
            escaped.append(QStringLiteral("\\\\"));
            escaped.append(character);
        } else if (character == QLatin1Char('%')) {
            escaped.append(QStringLiteral("%%"));
        } else {
            escaped.append(character);
        }
    }
    escaped.append(QLatin1Char('"'));
    return escaped;
}

QByteArray managedDesktopEntry(bool hidden,
                               QString *error) {
    const QString executable =
        QCoreApplication::applicationFilePath();
    if (!executablePathIsRepresentable(executable)) {
        *error = AutostartTranslations::tr(
            "The current GUI executable path cannot be used for autostart");
        return {};
    }
    const QString text = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=TRYX Panorama Manager\n"
        "Name[ru]=Менеджер TRYX Panorama\n"
        "Exec=%1 --autostart\n"
        "TryExec=%2\n"
        "Icon=tryx-panorama\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Hidden=%3\n"
        "X-TRYX-Panorama-Managed=true\n")
        .arg(escapeExecExecutable(executable),
             escapeDesktopString(executable),
             hidden ? QStringLiteral("true")
                    : QStringLiteral("false"));
    return text.toUtf8();
}

struct PinnedAutostartDirectory {
    ScopedFileDescriptor descriptor;
    FileIdentity identity;
    QString path;
};

struct PreparedEntry {
    QByteArray leafName;
    FileIdentity identity;
};

bool openPinnedAutostartDirectory(
    PinnedAutostartDirectory *directory,
    QString *error) {
    directory->path = autostartDirectoryPath();
    const QByteArray encoded = QFile::encodeName(directory->path);
    directory->descriptor = ScopedFileDescriptor(::open(
        encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat status {};
    if (directory->descriptor.get() < 0 ||
        ::fstat(directory->descriptor.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "The autostart directory permissions are unsafe"),
            directory->path);
        return false;
    }
    directory->identity.device = status.st_dev;
    directory->identity.inode = status.st_ino;
    return true;
}

bool pinnedDirectoryStillMatches(
    const PinnedAutostartDirectory &directory,
    QString *error) {
    const QByteArray encoded = QFile::encodeName(directory.path);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0 ||
        !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        status.st_dev != directory.identity.device ||
        status.st_ino != directory.identity.inode) {
        *error = AutostartTranslations::tr(
            "The GUI autostart entry changed during the operation");
        return false;
    }
    return true;
}

bool prepareEntry(const PinnedAutostartDirectory &directory,
                  const QByteArray &contents,
                  PreparedEntry *prepared,
                  QString *error) {
    int descriptor = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        const quint64 sequence =
            gTemporaryEntrySequence.fetch_add(
                1, std::memory_order_relaxed);
        prepared->leafName =
            QByteArrayLiteral(".tryx-panorama-manager.desktop.tmp.") +
            QByteArray::number(static_cast<qulonglong>(::getpid())) +
            '.' + QByteArray::number(
                      static_cast<qulonglong>(sequence));
        descriptor = ::openat(
            directory.descriptor.get(),
            prepared->leafName.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (descriptor >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (descriptor < 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to open the GUI autostart entry for writing"),
            userAutostartEntryPath());
        return false;
    }

    const auto discard = [&]() {
        ::close(descriptor);
        ::unlinkat(
            directory.descriptor.get(),
            prepared->leafName.constData(), 0);
    };
    qsizetype offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = ::write(
            descriptor, contents.constData() + offset,
            static_cast<size_t>(contents.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            discard();
            *error = pathError(
                AutostartTranslations::tr(
                    "Failed to write the GUI autostart entry"),
                userAutostartEntryPath());
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }

    struct stat status {};
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0 ||
        ::fsync(descriptor) != 0 ||
        ::fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_uid != ::getuid() ||
        status.st_nlink != 1 ||
        (status.st_mode & 0777) !=
            (S_IRUSR | S_IWUSR)) {
        discard();
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to write the GUI autostart entry"),
            userAutostartEntryPath());
        return false;
    }
    prepared->identity.device = status.st_dev;
    prepared->identity.inode = status.st_ino;
    ::close(descriptor);
    return true;
}

bool discardPreparedEntryIfMatches(
    const PinnedAutostartDirectory &directory,
    const PreparedEntry &prepared,
    const QByteArray &expectedContents);

bool renameEntryAt(int directoryDescriptor,
                   const QByteArray &from,
                   const QByteArray &to,
                   unsigned int flags) {
#if defined(SYS_renameat2)
    return ::syscall(
               SYS_renameat2,
               directoryDescriptor, from.constData(),
               directoryDescriptor, to.constData(),
               flags) == 0;
#else
    Q_UNUSED(directoryDescriptor);
    Q_UNUSED(from);
    Q_UNUSED(to);
    Q_UNUSED(flags);
    errno = ENOSYS;
    return false;
#endif
}

bool userEntryMatches(const UserEntry &expected,
                      const UserEntry &actual) {
    return expected.exists == actual.exists &&
           (!expected.exists ||
            (sameIdentity(expected.identity, actual.identity) &&
             expected.contents == actual.contents));
}

bool preparedEntryMatches(
    const PinnedAutostartDirectory &directory,
    const QByteArray &leafName,
    const PreparedEntry &prepared,
    const QByteArray &expectedContents) {
    const UserEntry actual = inspectUserEntryAt(
        directory.descriptor.get(), leafName,
        userAutostartEntryPath(), false);
    return actual.error.isEmpty() && actual.exists &&
           sameIdentity(prepared.identity, actual.identity) &&
           actual.contents == expectedContents;
}

bool discardPreparedEntryIfMatches(
    const PinnedAutostartDirectory &directory,
    const PreparedEntry &prepared,
    const QByteArray &expectedContents) {
    if (!preparedEntryMatches(
            directory, prepared.leafName,
            prepared, expectedContents)) {
        return false;
    }
    return ::unlinkat(
               directory.descriptor.get(),
               prepared.leafName.constData(), 0) == 0;
}

bool userEntryStillMatchesAt(
    const PinnedAutostartDirectory &directory,
    const QByteArray &leafName,
    const UserEntry &expected) {
    const UserEntry actual = inspectUserEntryAt(
        directory.descriptor.get(), leafName,
        userAutostartEntryPath(), false);
    return actual.error.isEmpty() &&
           userEntryMatches(expected, actual);
}

bool existingEntryMayBeChangedAt(
    const PinnedAutostartDirectory &directory,
    UserEntry *entry,
    QString *error) {
    *entry = inspectUserEntryAt(
        directory.descriptor.get(),
        kAutostartEntryName.toUtf8(),
        userAutostartEntryPath(), false);
    if (!entry->error.isEmpty()) {
        *error = entry->error;
        return false;
    }
    if (!entry->exists) {
        return true;
    }
    const DesktopEntry parsed =
        parseDesktopEntry(entry->contents, true);
    if (!parsed.error.isEmpty()) {
        *error = parsed.error;
        return false;
    }
    return true;
}

void setMutationChangedError(QString *error) {
    *error = AutostartTranslations::tr(
        "The GUI autostart entry changed during the operation");
}

bool writeManagedEntry(bool hidden, QString *error) {
    if (!ensureAutostartDirectory(error)) {
        return false;
    }
    PinnedAutostartDirectory directory;
    if (!openPinnedAutostartDirectory(&directory, error)) {
        return false;
    }
    UserEntry before;
    if (!existingEntryMayBeChangedAt(
            directory, &before, error)) {
        return false;
    }
    if (!before.exists) {
        const SystemEntry systemEntry = inspectSystemEntry();
        if (!systemEntry.error.isEmpty()) {
            *error = systemEntry.error;
            return false;
        }
    }
    const QByteArray contents = managedDesktopEntry(hidden, error);
    if (!error->isEmpty()) {
        return false;
    }
    PreparedEntry prepared;
    if (!prepareEntry(
            directory, contents, &prepared, error)) {
        return false;
    }

    const testing::LeafNamespaceMutation operation =
        before.exists
            ? testing::LeafNamespaceMutation::Replace
            : testing::LeafNamespaceMutation::Create;
    if (gBeforeLeafNamespaceMutationHook) {
        gBeforeLeafNamespaceMutationHook(
            operation, userAutostartEntryPath());
    }
    if (!pinnedDirectoryStillMatches(directory, error)) {
        discardPreparedEntryIfMatches(
            directory, prepared, contents);
        return false;
    }

    const QByteArray targetName =
        kAutostartEntryName.toUtf8();
    if (!preparedEntryMatches(
            directory, prepared.leafName,
            prepared, contents) ||
        !userEntryStillMatchesAt(
            directory, targetName, before)) {
        discardPreparedEntryIfMatches(
            directory, prepared, contents);
        setMutationChangedError(error);
        return false;
    }
    if (gAfterLeafPreconditionCheckHook) {
        gAfterLeafPreconditionCheckHook(
            operation, userAutostartEntryPath());
    }
    if (!before.exists) {
        if (!renameEntryAt(
                directory.descriptor.get(),
                prepared.leafName, targetName,
                RENAME_NOREPLACE)) {
            discardPreparedEntryIfMatches(
                directory, prepared, contents);
            setMutationChangedError(error);
            return false;
        }
        if (gAfterLeafNamespaceMutationHook) {
            gAfterLeafNamespaceMutationHook(
                operation, userAutostartEntryPath());
        }
        if (!preparedEntryMatches(
                directory, targetName,
                prepared, contents)) {
            setMutationChangedError(error);
            return false;
        }
    } else {
        if (!renameEntryAt(
                directory.descriptor.get(),
                prepared.leafName, targetName,
                RENAME_EXCHANGE)) {
            discardPreparedEntryIfMatches(
                directory, prepared, contents);
            setMutationChangedError(error);
            return false;
        }
        if (gAfterLeafNamespaceMutationHook) {
            gAfterLeafNamespaceMutationHook(
                operation, userAutostartEntryPath());
        }
        const UserEntry displaced = inspectUserEntryAt(
            directory.descriptor.get(), prepared.leafName,
            userAutostartEntryPath(), false);
        const bool targetIsPrepared = preparedEntryMatches(
            directory, targetName, prepared, contents);
        if (!targetIsPrepared || !displaced.error.isEmpty() ||
            !userEntryMatches(before, displaced)) {
            if (targetIsPrepared &&
                renameEntryAt(
                    directory.descriptor.get(),
                    prepared.leafName, targetName,
                    RENAME_EXCHANGE)) {
                discardPreparedEntryIfMatches(
                    directory, prepared, contents);
            }
            setMutationChangedError(error);
            return false;
        }
        if (!userEntryStillMatchesAt(
                directory, prepared.leafName, before) ||
            ::unlinkat(
                directory.descriptor.get(),
                prepared.leafName.constData(), 0) != 0) {
            *error = pathError(
                AutostartTranslations::tr(
                    "Failed to commit the GUI autostart entry atomically"),
                userAutostartEntryPath());
            return false;
        }
    }
    if (::fsync(directory.descriptor.get()) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to commit the GUI autostart entry atomically"),
            userAutostartEntryPath());
        return false;
    }
    return true;
}

bool removeManagedEntry(QString *error) {
    if (autostartDirectoryPath().isEmpty()) {
        *error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return false;
    }
    bool configMissing = false;
    if (!inspectUserConfigDirectory(
            false, &configMissing, error)) {
        return false;
    }
    if (configMissing) {
        return true;
    }
    bool directoryMissing = false;
    if (!inspectOwnedDirectory(
            autostartDirectoryPath(), &directoryMissing,
            error)) {
        return false;
    }
    if (directoryMissing) {
        return true;
    }
    PinnedAutostartDirectory directory;
    if (!openPinnedAutostartDirectory(&directory, error)) {
        return false;
    }
    UserEntry before;
    if (!existingEntryMayBeChangedAt(
            directory, &before, error)) {
        return false;
    }
    if (!before.exists) {
        return true;
    }

    PreparedEntry sentinel;
    if (!prepareEntry(
            directory, QByteArray{}, &sentinel, error)) {
        return false;
    }
    if (gBeforeLeafNamespaceMutationHook) {
        gBeforeLeafNamespaceMutationHook(
            testing::LeafNamespaceMutation::Remove,
            userAutostartEntryPath());
    }
    if (!pinnedDirectoryStillMatches(directory, error)) {
        discardPreparedEntryIfMatches(
            directory, sentinel, QByteArray{});
        return false;
    }

    const QByteArray targetName =
        kAutostartEntryName.toUtf8();
    if (!preparedEntryMatches(
            directory, sentinel.leafName,
            sentinel, QByteArray{}) ||
        !userEntryStillMatchesAt(
            directory, targetName, before)) {
        discardPreparedEntryIfMatches(
            directory, sentinel, QByteArray{});
        setMutationChangedError(error);
        return false;
    }
    if (!discardPreparedEntryIfMatches(
            directory, sentinel, QByteArray{})) {
        setMutationChangedError(error);
        return false;
    }
    if (gAfterLeafPreconditionCheckHook) {
        gAfterLeafPreconditionCheckHook(
            testing::LeafNamespaceMutation::Remove,
            userAutostartEntryPath());
    }
    if (!renameEntryAt(
            directory.descriptor.get(),
            targetName, sentinel.leafName,
            RENAME_NOREPLACE)) {
        setMutationChangedError(error);
        return false;
    }
    if (gAfterLeafNamespaceMutationHook) {
        gAfterLeafNamespaceMutationHook(
            testing::LeafNamespaceMutation::Remove,
            userAutostartEntryPath());
    }
    const UserEntry displaced = inspectUserEntryAt(
        directory.descriptor.get(), sentinel.leafName,
        userAutostartEntryPath(), false);
    if (!displaced.error.isEmpty() ||
        !userEntryMatches(before, displaced)) {
        renameEntryAt(
            directory.descriptor.get(),
            sentinel.leafName, targetName,
            RENAME_NOREPLACE);
        setMutationChangedError(error);
        return false;
    }
    const UserEntry current = inspectUserEntryAt(
        directory.descriptor.get(), targetName,
        userAutostartEntryPath(), false);
    if (!userEntryStillMatchesAt(
            directory, sentinel.leafName, before) ||
        ::unlinkat(
            directory.descriptor.get(),
            sentinel.leafName.constData(), 0) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to remove the GUI autostart entry"),
            userAutostartEntryPath());
        return false;
    }
    if (::fsync(directory.descriptor.get()) != 0) {
        *error = pathError(
            AutostartTranslations::tr(
                "Failed to remove the GUI autostart entry"),
            userAutostartEntryPath());
        return false;
    }
    if (!current.error.isEmpty() || current.exists) {
        setMutationChangedError(error);
        return false;
    }
    return true;
}

bool changeAutostartState(bool enabled, QString *error) {
    if (userConfigDirectory().isEmpty()) {
        *error = AutostartTranslations::tr(
            "The user configuration directory is unavailable");
        return false;
    }
    if (enabled) {
        return writeManagedEntry(false, error);
    }
    const SystemEntry systemEntry = inspectSystemEntry();
    if (!systemEntry.error.isEmpty()) {
        *error = systemEntry.error;
        return false;
    }
    return systemEntry.exists
        ? writeManagedEntry(true, error)
        : removeManagedEntry(error);
}

}  // namespace

State query() {
    const AutostartState state = queryAutostartState();
    return {state.available, state.enabled, state.error};
}

bool setEnabled(bool enabled, QString *error) {
    return changeAutostartState(enabled, error);
}

namespace testing {

void setUserConfigDirectoryOverride(const QString &directory) {
    gUserConfigDirectoryOverride = directory;
    gUserConfigDirectoryOverrideSet = true;
}

void clearUserConfigDirectoryOverride() {
    gUserConfigDirectoryOverride.clear();
    gUserConfigDirectoryOverrideSet = false;
}

void setBeforeUserEntryOpenHook(BeforeUserEntryOpenHook hook) {
    gBeforeUserEntryOpenHook = hook;
}

void clearBeforeUserEntryOpenHook() {
    gBeforeUserEntryOpenHook = nullptr;
}

void setBeforeLeafNamespaceMutationHook(
    BeforeLeafNamespaceMutationHook hook) {
    gBeforeLeafNamespaceMutationHook = hook;
}

void clearBeforeLeafNamespaceMutationHook() {
    gBeforeLeafNamespaceMutationHook = nullptr;
}

void setAfterLeafPreconditionCheckHook(
    AfterLeafPreconditionCheckHook hook) {
    gAfterLeafPreconditionCheckHook = hook;
}

void clearAfterLeafPreconditionCheckHook() {
    gAfterLeafPreconditionCheckHook = nullptr;
}

void setAfterLeafNamespaceMutationHook(
    AfterLeafNamespaceMutationHook hook) {
    gAfterLeafNamespaceMutationHook = hook;
}

void clearAfterLeafNamespaceMutationHook() {
    gAfterLeafNamespaceMutationHook = nullptr;
}

}  // namespace testing

}  // namespace gui_autostart
