#include "savedlayoutstore.h"

#include "applicationpaths.h"
#include "runtimeapplyrequestcodec.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Layout = TryxRuntimeSavedLayoutV1;
using MediaRef = TryxRuntimeSavedMediaRefV1;
using Store = tryx::SavedLayoutStore;

constexpr qint64 kMaximumStoreBytes = 1024LL * 1024LL;
constexpr qsizetype kMaximumLayoutsPerDevice = 32;
constexpr qsizetype kMaximumLayoutsTotal = 256;
constexpr qsizetype kMaximumLayoutNameCharacters = 80;
constexpr qsizetype kMaximumMediaNameCharacters = 128;
constexpr int kFormatVersion = 1;
constexpr char kIndexFileName[] = "index.json";

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
    ScopedFileDescriptor &operator=(const ScopedFileDescriptor &) = delete;

    int get() const { return descriptor_; }

private:
    int descriptor_ = -1;
};

QString systemError(const QString &prefix, int errorNumber) {
    return QStringLiteral("%1: %2")
        .arg(prefix, QString::fromLocal8Bit(std::strerror(errorNumber)));
}

bool directoryStatusIsSafe(const struct stat &status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool fileStatusIsSafe(const struct stat &status) {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           status.st_nlink == 1 &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool sameFileIdentity(const struct stat &left, const struct stat &right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool objectHasExactKeys(
    const QJsonObject &object, const QSet<QString> &expected) {
    const QStringList keys = object.keys();
    return keys.size() == expected.size() &&
           std::all_of(
               keys.cbegin(), keys.cend(),
               [&expected](const QString &key) {
                   return expected.contains(key);
               });
}

bool parseJsonInteger(
    const QJsonValue &value, quint64 minimum, quint64 maximum,
    quint64 *parsed) {
    if (!parsed || !value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!qIsFinite(number) || number < 0.0 ||
        number > static_cast<double>(maximum)) {
        return false;
    }
    const quint64 integer = static_cast<quint64>(number);
    if (static_cast<double>(integer) != number || integer < minimum ||
        integer > maximum) {
        return false;
    }
    *parsed = integer;
    return true;
}

bool parseCanonicalUnsigned(
    const QJsonValue &value, quint64 minimum, quint64 maximum,
    quint64 *parsed) {
    if (!parsed || !value.isString()) {
        return false;
    }
    const QString text = value.toString();
    if (text.isEmpty() ||
        (text.size() > 1 && text.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : text) {
        if (character < QLatin1Char('0') ||
            character > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const quint64 integer = text.toULongLong(&ok, 10);
    if (!ok || integer < minimum || integer > maximum ||
        QString::number(integer) != text) {
        return false;
    }
    *parsed = integer;
    return true;
}

bool isCanonicalUuid(const QString &value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces) == value;
}

bool isLowercaseSha256(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(
        value.cbegin(), value.cend(), [](const QChar character) {
            return (character >= QLatin1Char('0') &&
                    character <= QLatin1Char('9')) ||
                   (character >= QLatin1Char('a') &&
                    character <= QLatin1Char('f'));
        });
}

bool containsForbiddenTextCharacter(const QString &value) {
    const QList<uint> codePoints = value.toUcs4();
    return std::any_of(
        codePoints.cbegin(), codePoints.cend(), [](uint codePoint) {
            if (QChar::category(codePoint) == QChar::Other_Control) {
                return true;
            }
            switch (QChar::direction(codePoint)) {
            case QChar::DirLRE:
            case QChar::DirLRO:
            case QChar::DirRLE:
            case QChar::DirRLO:
            case QChar::DirPDF:
            case QChar::DirLRI:
            case QChar::DirRLI:
            case QChar::DirFSI:
            case QChar::DirPDI:
                return true;
            default:
                return false;
            }
        });
}

bool layoutNameIsValid(const QString &name) {
    const qsizetype characterCount = name.toUcs4().size();
    return !name.isEmpty() && name == name.trimmed() &&
           characterCount >= 1 &&
           characterCount <= kMaximumLayoutNameCharacters &&
           !containsForbiddenTextCharacter(name);
}

bool mediaNameIsValid(const QString &name) {
    if (name.isEmpty() || name.size() > kMaximumMediaNameCharacters ||
        name.startsWith(QLatin1Char('.')) ||
        name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\'))) {
        return false;
    }
    return std::all_of(
        name.cbegin(), name.cend(), [](const QChar character) {
            return character.isLetterOrNumber() ||
                   character == QLatin1Char('.') ||
                   character == QLatin1Char('_') ||
                   character == QLatin1Char('-');
        });
}

bool colorIsValid(const QString &color) {
    if (color.size() != 7 || color.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (qsizetype index = 1; index < color.size(); ++index) {
        const QChar character = color.at(index);
        if (!character.isDigit() &&
            !(character >= QLatin1Char('a') &&
              character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool styleIsValid(
    const QString &position, const QString &color,
    const QString &alignment) {
    return (position == QStringLiteral("Top") ||
            position == QStringLiteral("Bottom")) &&
           colorIsValid(color) &&
           (alignment == QStringLiteral("Left") ||
            alignment == QStringLiteral("Center") ||
            alignment == QStringLiteral("Right"));
}

bool valuesAreUniqueAndSupported(
    const QStringList &values, qsizetype maximum,
    const QSet<QString> &supported) {
    if (values.size() > maximum) {
        return false;
    }
    QSet<QString> seen;
    for (const QString &value : values) {
        if (!supported.contains(value) || seen.contains(value)) {
            return false;
        }
        seen.insert(value);
    }
    return true;
}

bool mediaRefIsValid(const MediaRef &media) {
    return media.schemaVersion == 1U &&
           isLowercaseSha256(media.mediaId) &&
           mediaNameIsValid(media.name) && media.size > 0 &&
           (media.source == 1U || media.source == 2U);
}

QString expectedMediaId(
    const QString &deviceIdentity, const MediaRef &media) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    hash.addData(deviceIdentity.toUtf8());
    hash.addData(separator);
    hash.addData(media.name.toUtf8());
    hash.addData(separator);
    hash.addData(QByteArray::number(media.size));
    hash.addData(separator);
    hash.addData(QByteArray::number(media.source));
    return QString::fromLatin1(hash.result().toHex());
}

bool requestIsValid(
    const TryxRuntimeApplyRequest &request,
    const QList<MediaRef> &media) {
    static const QSet<QString> supportedMetrics = [] {
        const QStringList catalog = tryxMetricsCatalog();
        return QSet<QString>(catalog.cbegin(), catalog.cend());
    }();
    static const QSet<QString> supportedBadges{
        QStringLiteral("CPU Badge"),
        QStringLiteral("GPU Badge")};

    const bool full =
        request.screenMode == QStringLiteral("Full Screen");
    const bool split = request.screenMode ==
        QStringLiteral("Screen Splitting");
    if ((!full && !split) || media.size() != (split ? 2 : 1) ||
        request.media.size() != media.size()) {
        return false;
    }
    QStringList expectedMedia;
    QSet<QString> mediaIds;
    QSet<QString> mediaNames;
    for (const MediaRef &entry : media) {
        if (!mediaRefIsValid(entry) ||
            mediaIds.contains(entry.mediaId) ||
            mediaNames.contains(entry.name)) {
            return false;
        }
        mediaIds.insert(entry.mediaId);
        mediaNames.insert(entry.name);
        expectedMedia.append(entry.name);
    }
    if (request.media != expectedMedia ||
        request.ratio != QStringLiteral("2:1") ||
        (split && request.playMode != QStringLiteral("Single")) ||
        (full && request.playMode != QStringLiteral("Single") &&
         request.playMode != QStringLiteral("Loop") &&
         request.playMode != QStringLiteral("Shuffle")) ||
        !valuesAreUniqueAndSupported(
            request.sysinfoLabels, 3, supportedMetrics) ||
        !valuesAreUniqueAndSupported(
            request.settingsBadges, 2, supportedBadges) ||
        !valuesAreUniqueAndSupported(
            request.sysinfoLabels2, 3, supportedMetrics) ||
        !valuesAreUniqueAndSupported(
            request.settingsBadges2, 2, supportedBadges) ||
        !styleIsValid(
            request.settingsPosition, request.settingsColor,
            request.settingsAlign)) {
        return false;
    }
    if (split) {
        if (!styleIsValid(
                request.settingsPosition2, request.settingsColor2,
                request.settingsAlign2)) {
            return false;
        }
    } else if (!request.sysinfoLabels2.isEmpty() ||
               !request.settingsBadges2.isEmpty() ||
               !request.settingsPosition2.isEmpty() ||
               !request.settingsColor2.isEmpty() ||
               !request.settingsAlign2.isEmpty()) {
        return false;
    }

    return request.filterOpacity == 0 && request.presetId.isEmpty() &&
           request.replaceOverlay && request.display.brightnessPresent &&
           request.display.brightness >= 0 &&
           request.display.brightness <= 100 &&
           !request.display.standbyPresent &&
           !request.display.standbyEnabled &&
           !request.display.backlightPresent &&
           request.display.backlightEnabled &&
           request.display.orientationPresent &&
           request.waterfallMode == request.display.waterfallMode;
}

bool layoutIsValid(const Layout &layout) {
    if (layout.schemaVersion != 1U ||
        !isCanonicalUuid(layout.layoutId) || layout.revision == 0 ||
        !tryxSavedLayoutDeviceIdentityIsCanonical(
            layout.deviceIdentity) ||
        !tryxSavedLayoutProductIdIsSupported(layout.productId) ||
        !layoutNameIsValid(layout.name) ||
        !requestIsValid(layout.request, layout.media)) {
        return false;
    }
    return std::all_of(
        layout.media.cbegin(), layout.media.cend(),
        [&layout](const MediaRef &media) {
            return media.mediaId ==
                   expectedMediaId(layout.deviceIdentity, media);
        });
}

QString layoutNameKey(const Layout &layout) {
    return layout.deviceIdentity + QChar(0) + layout.productId + QChar(0) +
           layout.name.toCaseFolded();
}

bool layoutCountLimitsAreValid(const QList<Layout> &layouts) {
    if (layouts.size() > kMaximumLayoutsTotal) {
        return false;
    }
    QHash<QString, qsizetype> deviceCounts;
    for (const Layout &layout : layouts) {
        const QString deviceScope =
            layout.deviceIdentity + QChar(0) + layout.productId;
        const qsizetype count = deviceCounts.value(deviceScope) + 1;
        if (count > kMaximumLayoutsPerDevice) {
            return false;
        }
        deviceCounts.insert(deviceScope, count);
    }
    return true;
}

bool stateIsValid(quint64 revision, const QList<Layout> &layouts) {
    if (revision == 0 || !layoutCountLimitsAreValid(layouts)) {
        return false;
    }
    QSet<QString> layoutIds;
    QSet<QString> layoutNames;
    QSet<quint64> recordRevisions;
    for (const Layout &layout : layouts) {
        if (!layoutIsValid(layout) || layout.revision > revision ||
            layoutIds.contains(layout.layoutId) ||
            layoutNames.contains(layoutNameKey(layout)) ||
            recordRevisions.contains(layout.revision)) {
            return false;
        }
        layoutIds.insert(layout.layoutId);
        layoutNames.insert(layoutNameKey(layout));
        recordRevisions.insert(layout.revision);
    }
    return true;
}

void sortLayouts(QList<Layout> *layouts) {
    if (!layouts) {
        return;
    }
    std::sort(
        layouts->begin(), layouts->end(),
        [](const Layout &left, const Layout &right) {
            if (left.deviceIdentity != right.deviceIdentity) {
                return left.deviceIdentity < right.deviceIdentity;
            }
            if (left.productId != right.productId) {
                return left.productId < right.productId;
            }
            const QString leftName = left.name.toCaseFolded();
            const QString rightName = right.name.toCaseFolded();
            if (leftName != rightName) {
                return leftName < rightName;
            }
            return left.layoutId < right.layoutId;
        });
}

QJsonObject mediaRefToJson(const MediaRef &media) {
    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"),
                  static_cast<qint64>(media.schemaVersion));
    object.insert(QStringLiteral("mediaId"), media.mediaId);
    object.insert(QStringLiteral("name"), media.name);
    object.insert(QStringLiteral("size"), QString::number(media.size));
    object.insert(QStringLiteral("source"),
                  static_cast<qint64>(media.source));
    object.insert(QStringLiteral("readOnly"), media.readOnly);
    return object;
}

QJsonObject layoutToJson(const Layout &layout) {
    QJsonArray media;
    for (const MediaRef &entry : layout.media) {
        media.append(mediaRefToJson(entry));
    }
    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"),
                  static_cast<qint64>(layout.schemaVersion));
    object.insert(QStringLiteral("layoutId"), layout.layoutId);
    object.insert(QStringLiteral("revision"),
                  QString::number(layout.revision));
    object.insert(QStringLiteral("deviceIdentity"), layout.deviceIdentity);
    object.insert(QStringLiteral("productId"), layout.productId);
    object.insert(QStringLiteral("name"), layout.name);
    object.insert(QStringLiteral("media"), media);
    object.insert(
        QStringLiteral("request"),
        tryx::runtime_apply_request_codec::runtimeApplyRequestToJson(
            layout.request));
    return object;
}

QByteArray serializedState(
    quint64 revision, const QList<Layout> &layouts) {
    QJsonArray serializedLayouts;
    for (const Layout &layout : layouts) {
        serializedLayouts.append(layoutToJson(layout));
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("revision"), QString::number(revision));
    root.insert(QStringLiteral("layouts"), serializedLayouts);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool requestFromJson(
    const QJsonObject &object, TryxRuntimeApplyRequest *request) {
    static const QSet<QString> requestKeys{
        QStringLiteral("media"),
        QStringLiteral("ratio"),
        QStringLiteral("screenMode"),
        QStringLiteral("playMode"),
        QStringLiteral("sysinfoLabels"),
        QStringLiteral("settingsPosition"),
        QStringLiteral("settingsColor"),
        QStringLiteral("settingsAlign"),
        QStringLiteral("settingsBadges"),
        QStringLiteral("filterOpacity"),
        QStringLiteral("presetId"),
        QStringLiteral("sysinfoLabels2"),
        QStringLiteral("settingsBadges2"),
        QStringLiteral("settingsPosition2"),
        QStringLiteral("settingsColor2"),
        QStringLiteral("settingsAlign2"),
        QStringLiteral("waterfallMode"),
        QStringLiteral("replaceOverlay"),
        QStringLiteral("display")};
    static const QSet<QString> displayKeys{
        QStringLiteral("brightnessPresent"),
        QStringLiteral("brightness"),
        QStringLiteral("standbyPresent"),
        QStringLiteral("standbyEnabled"),
        QStringLiteral("backlightPresent"),
        QStringLiteral("backlightEnabled"),
        QStringLiteral("orientationPresent"),
        QStringLiteral("mirrorMode"),
        QStringLiteral("waterfallMode")};
    if (!request || !objectHasExactKeys(object, requestKeys) ||
        !object.value(QStringLiteral("display")).isObject() ||
        !objectHasExactKeys(
            object.value(QStringLiteral("display")).toObject(),
            displayKeys)) {
        return false;
    }
    TryxRuntimeApplyRequest parsed;
    if (!tryx::runtime_apply_request_codec::runtimeApplyRequestFromJson(
            object, &parsed, true)) {
        return false;
    }
    parsed.settingsColor =
        object.value(QStringLiteral("settingsColor")).toString();
    parsed.settingsColor2 =
        object.value(QStringLiteral("settingsColor2")).toString();
    if (tryx::runtime_apply_request_codec::runtimeApplyRequestToJson(
            parsed) != object) {
        return false;
    }
    *request = parsed;
    return true;
}

bool mediaRefFromJson(const QJsonValue &value, MediaRef *media) {
    static const QSet<QString> keys{
        QStringLiteral("schemaVersion"),
        QStringLiteral("mediaId"),
        QStringLiteral("name"),
        QStringLiteral("size"),
        QStringLiteral("source"),
        QStringLiteral("readOnly")};
    if (!media || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    quint64 schemaVersion = 0;
    quint64 size = 0;
    quint64 source = 0;
    if (!objectHasExactKeys(object, keys) ||
        !parseJsonInteger(
            object.value(QStringLiteral("schemaVersion")), 1, 1,
            &schemaVersion) ||
        !object.value(QStringLiteral("mediaId")).isString() ||
        !object.value(QStringLiteral("name")).isString() ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("size")), 1,
            std::numeric_limits<quint64>::max(), &size) ||
        !parseJsonInteger(
            object.value(QStringLiteral("source")), 1, 2, &source) ||
        !object.value(QStringLiteral("readOnly")).isBool()) {
        return false;
    }
    MediaRef parsed;
    parsed.schemaVersion = static_cast<quint32>(schemaVersion);
    parsed.mediaId = object.value(QStringLiteral("mediaId")).toString();
    parsed.name = object.value(QStringLiteral("name")).toString();
    parsed.size = size;
    parsed.source = static_cast<quint32>(source);
    parsed.readOnly = object.value(QStringLiteral("readOnly")).toBool();
    if (!mediaRefIsValid(parsed)) {
        return false;
    }
    *media = parsed;
    return true;
}

bool layoutFromJson(const QJsonValue &value, Layout *layout) {
    static const QSet<QString> keys{
        QStringLiteral("schemaVersion"),
        QStringLiteral("layoutId"),
        QStringLiteral("revision"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("productId"),
        QStringLiteral("name"),
        QStringLiteral("media"),
        QStringLiteral("request")};
    if (!layout || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    quint64 schemaVersion = 0;
    quint64 revision = 0;
    if (!objectHasExactKeys(object, keys) ||
        !parseJsonInteger(
            object.value(QStringLiteral("schemaVersion")), 1, 1,
            &schemaVersion) ||
        !object.value(QStringLiteral("layoutId")).isString() ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("revision")), 1,
            std::numeric_limits<quint64>::max(), &revision) ||
        !object.value(QStringLiteral("deviceIdentity")).isString() ||
        !object.value(QStringLiteral("productId")).isString() ||
        !object.value(QStringLiteral("name")).isString() ||
        !object.value(QStringLiteral("media")).isArray() ||
        !object.value(QStringLiteral("request")).isObject()) {
        return false;
    }
    const QJsonArray mediaArray =
        object.value(QStringLiteral("media")).toArray();
    if (mediaArray.size() < 1 || mediaArray.size() > 2) {
        return false;
    }
    Layout parsed;
    parsed.schemaVersion = static_cast<quint32>(schemaVersion);
    parsed.layoutId = object.value(QStringLiteral("layoutId")).toString();
    parsed.revision = revision;
    parsed.deviceIdentity =
        object.value(QStringLiteral("deviceIdentity")).toString();
    parsed.productId = object.value(QStringLiteral("productId")).toString();
    parsed.name = object.value(QStringLiteral("name")).toString();
    parsed.media.reserve(mediaArray.size());
    for (const QJsonValue &mediaValue : mediaArray) {
        MediaRef media;
        if (!mediaRefFromJson(mediaValue, &media)) {
            return false;
        }
        parsed.media.append(media);
    }
    if (!requestFromJson(
            object.value(QStringLiteral("request")).toObject(),
            &parsed.request) ||
        !layoutIsValid(parsed)) {
        return false;
    }
    *layout = parsed;
    return true;
}

Store::MutationResult mutationFailure(
    Store::ErrorCode code, const QString &detail, quint64 revision,
    bool commitMayExist = false) {
    Store::MutationResult result;
    result.code = code;
    result.detail = detail;
    result.revision = revision;
    result.commitMayExist = commitMayExist;
    return result;
}

}  // namespace

namespace tryx {

SavedLayoutStore::SavedLayoutStore(QString directory)
    : directory_(directory.trimmed().isEmpty()
          ? QDir(panorama::sharedApplicationDataLocation())
                .filePath(QStringLiteral("saved-layouts"))
          : QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}

QString SavedLayoutStore::directory() const {
    return directory_;
}

QString SavedLayoutStore::indexPath() const {
    return QDir(directory_).filePath(QString::fromLatin1(kIndexFileName));
}

bool SavedLayoutStore::writesEnabled() const {
    return writesEnabled_;
}

quint64 SavedLayoutStore::revision() const {
    return revision_;
}

QList<TryxRuntimeSavedLayoutV1> SavedLayoutStore::layouts() const {
    return layouts_;
}

bool SavedLayoutStore::layoutIsCanonical(
    const TryxRuntimeSavedLayoutV1 &layout, QString *detail) {
    if (detail) {
        detail->clear();
    }
    if (::layoutIsValid(layout)) {
        return true;
    }
    if (detail) {
        *detail = QStringLiteral("The saved layout is invalid");
    }
    return false;
}

void SavedLayoutStore::disableWrites(const QString &diagnostic) {
    writesEnabled_ = false;
    diagnostic_ = diagnostic.left(512);
}

SavedLayoutStore::LoadResult SavedLayoutStore::load() {
    writesEnabled_ = true;
    revision_ = 0;
    layouts_.clear();
    diagnostic_.clear();

    LoadResult result;
    const auto reject =
        [this, &result](LoadStatus status, const QString &detail) {
            revision_ = 0;
            layouts_.clear();
            disableWrites(detail);
            result.status = status;
            result.writesEnabled = false;
            result.revision = 0;
            result.layouts.clear();
            result.detail = diagnostic_;
            return result;
        };

    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    const ScopedFileDescriptor directoryDescriptor(::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        return reject(
            errorNumber == ELOOP || errorNumber == ENOTDIR
                ? LoadStatus::IgnoredUnsafe
                : LoadStatus::ReadFailed,
            systemError(
                QStringLiteral("Cannot open the saved layouts directory"),
                errorNumber));
    }

    struct stat directoryStatus {};
    if (::fstat(directoryDescriptor.get(), &directoryStatus) != 0) {
        return reject(
            LoadStatus::ReadFailed,
            systemError(
                QStringLiteral("Cannot inspect the saved layouts directory"),
                errno));
    }
    if (!directoryStatusIsSafe(directoryStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral("Ignoring an unsafe saved layouts directory"));
    }

    const ScopedFileDescriptor indexDescriptor(::openat(
        directoryDescriptor.get(), kIndexFileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (indexDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        return reject(
            errorNumber == ELOOP || errorNumber == ENOTDIR
                ? LoadStatus::IgnoredUnsafe
                : LoadStatus::ReadFailed,
            systemError(
                QStringLiteral("Cannot open the saved layouts index"),
                errorNumber));
    }

    struct stat indexStatus {};
    if (::fstat(indexDescriptor.get(), &indexStatus) != 0) {
        return reject(
            LoadStatus::ReadFailed,
            systemError(
                QStringLiteral("Cannot inspect the saved layouts index"),
                errno));
    }
    if (!fileStatusIsSafe(indexStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral("Ignoring an unsafe saved layouts index"));
    }
    if (indexStatus.st_size < 0 ||
        indexStatus.st_size > kMaximumStoreBytes) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            QStringLiteral("The saved layouts index exceeds its size limit"));
    }

    QByteArray payload;
    payload.reserve(static_cast<qsizetype>(indexStatus.st_size));
    char buffer[16 * 1024];
    while (true) {
        const ssize_t readSize =
            ::read(indexDescriptor.get(), buffer, sizeof(buffer));
        if (readSize < 0 && errno == EINTR) {
            continue;
        }
        if (readSize < 0) {
            return reject(
                LoadStatus::ReadFailed,
                systemError(
                    QStringLiteral("Cannot read the saved layouts index"),
                    errno));
        }
        if (readSize == 0) {
            break;
        }
        if (payload.size() + readSize > kMaximumStoreBytes) {
            return reject(
                LoadStatus::ResourceLimitExceeded,
                QStringLiteral(
                    "The saved layouts index exceeds its size limit"));
        }
        payload.append(buffer, static_cast<qsizetype>(readSize));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring a malformed saved layouts index"));
    }

    static const QSet<QString> rootKeys{
        QStringLiteral("version"),
        QStringLiteral("revision"),
        QStringLiteral("layouts")};
    const QJsonObject root = document.object();
    quint64 version = 0;
    if (!root.value(QStringLiteral("version")).isDouble() ||
        !parseJsonInteger(
            root.value(QStringLiteral("version")), 0,
            std::numeric_limits<quint32>::max(), &version)) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring an invalid saved layouts version"));
    }
    if (version != kFormatVersion) {
        return reject(
            version > kFormatVersion
                ? LoadStatus::UnsupportedVersion
                : LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring an unsupported saved layouts version"));
    }
    if (!objectHasExactKeys(root, rootKeys) ||
        !root.value(QStringLiteral("layouts")).isArray()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring malformed saved layouts fields"));
    }

    quint64 loadedRevision = 0;
    if (!parseCanonicalUnsigned(
            root.value(QStringLiteral("revision")), 1,
            std::numeric_limits<quint64>::max(), &loadedRevision)) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring an invalid saved layouts revision"));
    }
    const QJsonArray serializedLayouts =
        root.value(QStringLiteral("layouts")).toArray();
    if (serializedLayouts.size() > kMaximumLayoutsTotal) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            QStringLiteral("The saved layouts index has too many records"));
    }

    QList<Layout> loadedLayouts;
    loadedLayouts.reserve(serializedLayouts.size());
    for (const QJsonValue &value : serializedLayouts) {
        Layout layout;
        if (!layoutFromJson(value, &layout)) {
            return reject(
                LoadStatus::IgnoredMalformed,
                QStringLiteral("Ignoring an invalid saved layout record"));
        }
        loadedLayouts.append(layout);
    }
    if (!layoutCountLimitsAreValid(loadedLayouts)) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            QStringLiteral(
                "The saved layouts index exceeds its record limits"));
    }
    if (!stateIsValid(loadedRevision, loadedLayouts)) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral("Ignoring inconsistent saved layouts state"));
    }

    struct stat liveIndexStatus {};
    if (::fstatat(
            directoryDescriptor.get(), kIndexFileName,
            &liveIndexStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !fileStatusIsSafe(liveIndexStatus) ||
        !sameFileIdentity(indexStatus, liveIndexStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral(
                "The saved layouts index changed during loading"));
    }
    struct stat liveDirectoryStatus {};
    if (::lstat(encodedDirectory.constData(), &liveDirectoryStatus) != 0 ||
        !directoryStatusIsSafe(liveDirectoryStatus) ||
        !sameFileIdentity(directoryStatus, liveDirectoryStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral(
                "The saved layouts directory changed during loading"));
    }

    sortLayouts(&loadedLayouts);
    revision_ = loadedRevision;
    layouts_ = loadedLayouts;
    result.status = LoadStatus::Loaded;
    result.revision = revision_;
    result.layouts = layouts_;
    return result;
}

TryxRuntimeSavedLayoutsSnapshotV1 SavedLayoutStore::snapshot(
    const QString &deviceIdentity, const QString &productId) const {
    TryxRuntimeSavedLayoutsSnapshotV1 result;
    result.revision = revision_;
    result.deviceIdentity = deviceIdentity;
    result.productId = productId;
    if (!writesEnabled_) {
        result.status = QStringLiteral("Unavailable");
        result.diagnostic = diagnostic_.isEmpty()
            ? QStringLiteral("Saved layouts are unavailable")
            : diagnostic_;
        return result;
    }
    if (deviceIdentity.isEmpty()) {
        result.status = QStringLiteral("Disconnected");
        return result;
    }
    if (!tryxSavedLayoutDeviceIdentityIsCanonical(deviceIdentity)) {
        result.status = QStringLiteral("Unavailable");
        result.diagnostic = QStringLiteral(
            "The saved layout device identity is invalid");
        return result;
    }
    if (!tryxSavedLayoutProductIdIsSupported(productId)) {
        result.status = QStringLiteral("Unsupported");
        return result;
    }
    result.status = QStringLiteral("Ready");
    for (const Layout &layout : layouts_) {
        if (layout.deviceIdentity == deviceIdentity &&
            layout.productId == productId) {
            result.layouts.append(layout);
        }
    }
    return result;
}

bool SavedLayoutStore::ensureDirectory(QString *detail) const {
    if (directory_.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral(
                "The saved layouts directory is unavailable");
        }
        return false;
    }
    QFileInfo info(directory_);
    if (info.isSymLink() || (info.exists() && !info.isDir())) {
        if (detail) {
            *detail = QStringLiteral(
                "The saved layouts path is not a direct directory");
        }
        return false;
    }
    const bool create = !info.exists();
    if (create && !QDir().mkpath(directory_)) {
        if (detail) {
            *detail = QStringLiteral(
                "Cannot create the saved layouts directory");
        }
        return false;
    }
    if (create && !QFile::setPermissions(
            directory_, QFileDevice::ReadOwner |
                QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        if (detail) {
            *detail = QStringLiteral(
                "Cannot secure the saved layouts directory");
        }
        return false;
    }
    info.refresh();
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        if (detail) {
            *detail = QStringLiteral(
                "The saved layouts path is not a direct directory");
        }
        return false;
    }
    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    struct stat status {};
    if (::lstat(encodedDirectory.constData(), &status) != 0 ||
        !directoryStatusIsSafe(status)) {
        if (detail) {
            *detail = QStringLiteral(
                "The saved layouts directory is not owned safely");
        }
        return false;
    }
    return true;
}

bool SavedLayoutStore::destinationPathIsSafe(
    int directoryDescriptor, QString *detail) const {
    struct stat status {};
    if (::fstatat(
            directoryDescriptor, kIndexFileName, &status,
            AT_SYMLINK_NOFOLLOW) != 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return true;
        }
        if (detail) {
            *detail = systemError(
                QStringLiteral("Cannot inspect the saved layouts index"),
                errorNumber);
        }
        return false;
    }
    if (fileStatusIsSafe(status)) {
        return true;
    }
    if (detail) {
        *detail = QStringLiteral("The saved layouts index path is unsafe");
    }
    return false;
}

SavedLayoutStore::PersistResult SavedLayoutStore::persist(
    quint64 revision, const QList<TryxRuntimeSavedLayoutV1> &layouts) {
    if (!stateIsValid(revision, layouts)) {
        return {
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layouts state is invalid"), false};
    }
    const QByteArray payload = serializedState(revision, layouts);
    if (payload.isEmpty() || payload.size() > kMaximumStoreBytes) {
        return {
            ErrorCode::ResourceLimitExceeded,
            QStringLiteral("The saved layouts state exceeds its size limit"),
            false};
    }

    QString safetyError;
    if (!ensureDirectory(&safetyError)) {
        disableWrites(safetyError);
        return {ErrorCode::DirectoryUnavailable, safetyError, false};
    }

    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    const ScopedFileDescriptor directoryDescriptor(::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        const QString error = systemError(
            QStringLiteral(
                "Cannot open the saved layouts directory for writing"),
            errno);
        disableWrites(error);
        return {ErrorCode::DirectoryUnavailable, error, false};
    }

    struct stat openedDirectoryStatus {};
    if (::fstat(directoryDescriptor.get(), &openedDirectoryStatus) != 0 ||
        !directoryStatusIsSafe(openedDirectoryStatus)) {
        const QString error = QStringLiteral(
            "The saved layouts directory is not owned safely");
        disableWrites(error);
        return {ErrorCode::UnsafePath, error, false};
    }
    if (!destinationPathIsSafe(
            directoryDescriptor.get(), &safetyError)) {
        disableWrites(safetyError);
        return {ErrorCode::UnsafePath, safetyError, false};
    }

    const QString descriptorBoundPath = QStringLiteral(
        "/proc/self/fd/%1/%2")
        .arg(directoryDescriptor.get())
        .arg(QString::fromLatin1(kIndexFileName));
    QSaveFile file(descriptorBoundPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return {ErrorCode::WriteFailed, file.errorString(), false};
    }
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size()) {
        const QString error = file.errorString().isEmpty()
            ? QStringLiteral("Cannot write the saved layouts index")
            : file.errorString();
        file.cancelWriting();
        return {ErrorCode::WriteFailed, error, false};
    }
    if (!file.commit()) {
        return {ErrorCode::CommitFailed, file.errorString(), false};
    }

#ifdef TRYX_SAVED_LAYOUT_STORE_TESTING
    if (afterCommitHookForTesting_) {
        const auto hook = afterCommitHookForTesting_;
        afterCommitHookForTesting_ = {};
        hook();
    }
#endif

    struct stat committedFileStatus {};
    if (::fstatat(
            directoryDescriptor.get(), kIndexFileName,
            &committedFileStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !fileStatusIsSafe(committedFileStatus)) {
        const QString error = QStringLiteral(
            "The committed saved layouts index cannot be confirmed safely");
        disableWrites(error);
        return {ErrorCode::CommitUnknown, error, true};
    }
    struct stat liveDirectoryStatus {};
    if (::lstat(encodedDirectory.constData(), &liveDirectoryStatus) != 0 ||
        !directoryStatusIsSafe(liveDirectoryStatus) ||
        !sameFileIdentity(openedDirectoryStatus, liveDirectoryStatus)) {
        const QString error = QStringLiteral(
            "The saved layouts directory changed during the atomic write");
        disableWrites(error);
        return {ErrorCode::CommitUnknown, error, true};
    }
    return {};
}

SavedLayoutStore::MutationResult SavedLayoutStore::put(
    quint64 expectedRevision,
    const TryxRuntimeSavedLayoutV1 &sourceLayout) {
    if (!writesEnabled_) {
        return mutationFailure(
            ErrorCode::WritesDisabled,
            QStringLiteral("Saved layout writes are disabled"), revision_);
    }
    if (expectedRevision != revision_) {
        return mutationFailure(
            ErrorCode::RevisionConflict,
            QStringLiteral("The saved layouts revision is stale"),
            revision_);
    }
    if (revision_ == std::numeric_limits<quint64>::max()) {
        return mutationFailure(
            ErrorCode::ResourceLimitExceeded,
            QStringLiteral("The saved layouts revision is exhausted"),
            revision_);
    }

    Layout candidate = sourceLayout;
    if (candidate.layoutId.isEmpty()) {
        if (candidate.revision != 0) {
            return mutationFailure(
                ErrorCode::RevisionConflict,
                QStringLiteral(
                    "A new saved layout must have revision zero"),
                revision_);
        }
        candidate.layoutId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    } else if (!isCanonicalUuid(candidate.layoutId)) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layout ID is invalid"), revision_);
    }

    qsizetype existingIndex = -1;
    for (qsizetype index = 0; index < layouts_.size(); ++index) {
        if (layouts_.at(index).layoutId == candidate.layoutId) {
            existingIndex = index;
            break;
        }
    }
    if (existingIndex >= 0) {
        const Layout &existing = layouts_.at(existingIndex);
        if (candidate.revision != existing.revision) {
            return mutationFailure(
                ErrorCode::RevisionConflict,
                QStringLiteral("The saved layout record revision is stale"),
                revision_);
        }
        if (candidate.deviceIdentity != existing.deviceIdentity ||
            candidate.productId != existing.productId) {
            return mutationFailure(
                ErrorCode::InvalidInput,
                QStringLiteral(
                    "A saved layout cannot move between devices"),
                revision_);
        }
        if (candidate.name != existing.name) {
            return mutationFailure(
                ErrorCode::InvalidInput,
                QStringLiteral(
                    "Renaming a saved layout is not supported"),
                revision_);
        }
    } else if (candidate.revision != 0) {
        return mutationFailure(
            ErrorCode::RevisionConflict,
            QStringLiteral(
                "An unknown saved layout must have revision zero"),
            revision_);
    }

    const quint64 nextRevision = revision_ + 1;
    candidate.revision = nextRevision;
    if (!layoutIsValid(candidate)) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layout is invalid"), revision_);
    }

    const QString candidateNameKey = layoutNameKey(candidate);
    for (const Layout &layout : layouts_) {
        if (layout.layoutId != candidate.layoutId &&
            layoutNameKey(layout) == candidateNameKey) {
            return mutationFailure(
                ErrorCode::NameConflict,
                QStringLiteral(
                    "A saved layout with this name already exists"),
                revision_);
        }
    }

    if (existingIndex < 0) {
        if (layouts_.size() >= kMaximumLayoutsTotal) {
            return mutationFailure(
                ErrorCode::ResourceLimitExceeded,
                QStringLiteral("The saved layouts limit was reached"),
                revision_);
        }
        const qsizetype deviceCount = std::count_if(
            layouts_.cbegin(), layouts_.cend(),
            [&candidate](const Layout &layout) {
                return layout.deviceIdentity == candidate.deviceIdentity &&
                       layout.productId == candidate.productId;
            });
        if (deviceCount >= kMaximumLayoutsPerDevice) {
            return mutationFailure(
                ErrorCode::ResourceLimitExceeded,
                QStringLiteral(
                    "The saved layouts limit for this device was reached"),
                revision_);
        }
    }

    QList<Layout> candidateLayouts = layouts_;
    if (existingIndex >= 0) {
        candidateLayouts[existingIndex] = candidate;
    } else {
        candidateLayouts.append(candidate);
    }
    sortLayouts(&candidateLayouts);
    if (!stateIsValid(nextRevision, candidateLayouts)) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layouts state is inconsistent"),
            revision_);
    }

    const PersistResult persisted = persist(nextRevision, candidateLayouts);
    if (!persisted.ok()) {
        return mutationFailure(
            persisted.code, persisted.detail, revision_,
            persisted.commitMayExist);
    }

    revision_ = nextRevision;
    layouts_ = candidateLayouts;
    MutationResult result;
    result.revision = revision_;
    result.layout = candidate;
    return result;
}

SavedLayoutStore::MutationResult SavedLayoutStore::remove(
    quint64 expectedRevision, const QString &deviceIdentity,
    const QString &productId, const QString &layoutId) {
    if (!writesEnabled_) {
        return mutationFailure(
            ErrorCode::WritesDisabled,
            QStringLiteral("Saved layout writes are disabled"), revision_);
    }
    if (expectedRevision != revision_) {
        return mutationFailure(
            ErrorCode::RevisionConflict,
            QStringLiteral("The saved layouts revision is stale"),
            revision_);
    }
    if (!tryxSavedLayoutDeviceIdentityIsCanonical(deviceIdentity) ||
        !tryxSavedLayoutProductIdIsSupported(productId) ||
        !isCanonicalUuid(layoutId)) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layout delete request is invalid"),
            revision_);
    }

    qsizetype existingIndex = -1;
    for (qsizetype index = 0; index < layouts_.size(); ++index) {
        if (layouts_.at(index).layoutId == layoutId) {
            existingIndex = index;
            break;
        }
    }
    if (existingIndex < 0) {
        return mutationFailure(
            ErrorCode::NotFound,
            QStringLiteral("The saved layout does not exist"), revision_);
    }
    const Layout removedLayout = layouts_.at(existingIndex);
    if (removedLayout.deviceIdentity != deviceIdentity ||
        removedLayout.productId != productId) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "The saved layout belongs to another device scope"),
            revision_);
    }
    if (revision_ == std::numeric_limits<quint64>::max()) {
        return mutationFailure(
            ErrorCode::ResourceLimitExceeded,
            QStringLiteral("The saved layouts revision is exhausted"),
            revision_);
    }

    const quint64 nextRevision = revision_ + 1;
    QList<Layout> candidateLayouts = layouts_;
    candidateLayouts.removeAt(existingIndex);
    if (!stateIsValid(nextRevision, candidateLayouts)) {
        return mutationFailure(
            ErrorCode::InvalidInput,
            QStringLiteral("The saved layouts state is inconsistent"),
            revision_);
    }
    const PersistResult persisted = persist(nextRevision, candidateLayouts);
    if (!persisted.ok()) {
        return mutationFailure(
            persisted.code, persisted.detail, revision_,
            persisted.commitMayExist);
    }

    revision_ = nextRevision;
    layouts_ = candidateLayouts;
    MutationResult result;
    result.revision = revision_;
    result.layout = removedLayout;
    return result;
}

}  // namespace tryx
