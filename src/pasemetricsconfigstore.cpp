#include "pasemetricsconfigstore.h"

#include "applicationpaths.h"
#include "devicemanagermessages.h"
#include "paseoverlayconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using tryx::pase_overlay_config::hasDuplicateMetricLabels;
using tryx::pase_overlay_config::hasDuplicateValues;
using tryx::pase_overlay_config::isSupportedPaseBadge;
using tryx::pase_overlay_config::isSupportedPaseMetricLabel;
using tryx::pase_overlay_config::paseOverlayHasContent;

constexpr int kFormatVersion = 2;
constexpr qint64 kMaximumConfigBytes = 64LL * 1024LL;
constexpr qsizetype kMaximumDeviceSerialLength = 256;

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

QString systemErrorText(const QString &translatedPrefix, int errorNumber) {
    return QStringLiteral("%1: %2")
        .arg(translatedPrefix,
             QString::fromLocal8Bit(std::strerror(errorNumber)));
}

tryx::PaseMetricsConfigStore::MutationResult successResult() {
    return {};
}

tryx::PaseMetricsConfigStore::MutationResult failureResult(
    tryx::PaseMetricsConfigStore::ErrorCode code,
    const QString &detail) {
    return {code, detail};
}

QString normalizedDeviceSerial(const QString &deviceSerial) {
    return deviceSerial.trimmed();
}

bool deviceSerialIsValid(const QString &deviceSerial) {
    const QString normalized = normalizedDeviceSerial(deviceSerial);
    return !normalized.isEmpty() &&
           normalized.size() <= kMaximumDeviceSerialLength;
}

bool overlayAreaIsValid(
    const PrinterProtocol::PaseOverlayAreaConfig &area) {
    if (area.metrics.size() > 3 || area.badges.size() > 2 ||
        area.textColor > 0x00FFFFFFU ||
        hasDuplicateMetricLabels(area.metrics) ||
        hasDuplicateValues(area.badges)) {
        return false;
    }
    for (const QString &metric : area.metrics) {
        if (!isSupportedPaseMetricLabel(metric)) {
            return false;
        }
    }
    for (const QString &badge : area.badges) {
        if (!isSupportedPaseBadge(badge)) {
            return false;
        }
    }
    const bool alignmentValid =
        area.alignment == QStringLiteral("Left") ||
        area.alignment == QStringLiteral("Center") ||
        area.alignment == QStringLiteral("Right");
    const bool placementValid =
        area.verticalPlacement == QStringLiteral("Top") ||
        area.verticalPlacement == QStringLiteral("Bottom");
    return alignmentValid && placementValid;
}

void clearTransientAreaValues(
    PrinterProtocol::PaseOverlayAreaConfig *area) {
    if (!area) {
        return;
    }
    area->initialLabels.clear();
    area->initialValues.clear();
    area->initialUnits.clear();
}

PrinterProtocol::PaseOverlayConfig canonicalOverlay(
    const PrinterProtocol::PaseOverlayConfig &source) {
    PrinterProtocol::PaseOverlayConfig overlay = source;
    clearTransientAreaValues(&overlay.left);
    clearTransientAreaValues(&overlay.right);
    overlay.cpuBadgeText.clear();
    overlay.gpuBadgeText.clear();
    if (!overlay.dualMode) {
        overlay.right = {};
    }
    return overlay;
}

bool overlayIsValid(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return paseOverlayHasContent(overlay) &&
           overlayAreaIsValid(overlay.left) &&
           (!overlay.dualMode || overlayAreaIsValid(overlay.right));
}

bool parseStringArray(const QJsonValue &value, qsizetype maximumSize,
                      QStringList *output) {
    if (!output || !value.isArray()) {
        return false;
    }
    const QJsonArray values = value.toArray();
    if (values.size() > maximumSize) {
        return false;
    }
    QStringList parsed;
    parsed.reserve(values.size());
    for (const QJsonValue &entry : values) {
        if (!entry.isString()) {
            return false;
        }
        parsed.append(entry.toString());
    }
    *output = parsed;
    return true;
}

bool parseArea(const QJsonValue &value,
               PrinterProtocol::PaseOverlayAreaConfig *area) {
    if (!area || !value.isObject()) {
        return false;
    }
    const QJsonObject source = value.toObject();
    if (!source.value(QStringLiteral("alignment")).isString() ||
        !source.value(QStringLiteral("textColor")).isDouble() ||
        !source.value(QStringLiteral("verticalPlacement")).isString()) {
        return false;
    }
    const qint64 textColor =
        source.value(QStringLiteral("textColor")).toInteger(-1);
    if (textColor < 0 || textColor > 0x00FFFFFF) {
        return false;
    }

    PrinterProtocol::PaseOverlayAreaConfig parsed;
    if (!parseStringArray(source.value(QStringLiteral("metrics")), 3,
                          &parsed.metrics) ||
        !parseStringArray(source.value(QStringLiteral("badges")), 2,
                          &parsed.badges)) {
        return false;
    }
    parsed.alignment =
        source.value(QStringLiteral("alignment")).toString();
    parsed.textColor = static_cast<quint32>(textColor);
    parsed.verticalPlacement =
        source.value(QStringLiteral("verticalPlacement")).toString();
    if (!overlayAreaIsValid(parsed)) {
        return false;
    }
    *area = parsed;
    return true;
}

QJsonObject areaToJson(
    const PrinterProtocol::PaseOverlayAreaConfig &area) {
    QJsonObject object;
    object.insert(QStringLiteral("metrics"),
                  QJsonArray::fromStringList(area.metrics));
    object.insert(QStringLiteral("badges"),
                  QJsonArray::fromStringList(area.badges));
    object.insert(QStringLiteral("alignment"), area.alignment);
    object.insert(QStringLiteral("textColor"),
                  static_cast<qint64>(area.textColor));
    object.insert(QStringLiteral("verticalPlacement"),
                  area.verticalPlacement);
    return object;
}

QByteArray serializedPayload(
    const QString &deviceSerial,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("enabled"), true);
    root.insert(QStringLiteral("deviceSerial"), deviceSerial);
    root.insert(QStringLiteral("dualMode"), overlay.dualMode);
    root.insert(QStringLiteral("waterfallMode"), overlay.waterfallMode);
    root.insert(QStringLiteral("left"), areaToJson(overlay.left));
    root.insert(QStringLiteral("right"), areaToJson(overlay.right));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

}  // namespace

namespace tryx {

PaseMetricsConfigStore::PaseMetricsConfigStore(QString directory)
    : directory_(directory.trimmed().isEmpty()
          ? panorama::sharedApplicationDataLocation()
          : QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}

QString PaseMetricsConfigStore::directory() const {
    return directory_;
}

QString PaseMetricsConfigStore::configPath() const {
    return QDir(directory_).filePath(QStringLiteral("pase-metrics.json"));
}

bool PaseMetricsConfigStore::writesEnabled() const {
    return writesEnabled_;
}

bool PaseMetricsConfigStore::ensureDirectory(
    QString *errorMessage) const {
    QFileInfo info(directory_);
    if (info.isSymLink() || (info.exists() && !info.isDir())) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The PASE metrics configuration path is not a direct directory");
        }
        return false;
    }
    if (!info.exists() && !QDir().mkpath(directory_)) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Cannot create the PASE metrics configuration directory");
        }
        return false;
    }
    info.refresh();
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The PASE metrics configuration path is not a direct directory");
        }
        return false;
    }
    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    struct stat status {};
    if (::lstat(encodedDirectory.constData(), &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The PASE metrics configuration directory is not owned safely");
        }
        return false;
    }
    return true;
}

bool PaseMetricsConfigStore::destinationPathIsSafe(
    QString *errorMessage) const {
    const QByteArray encodedPath = QFile::encodeName(configPath());
    struct stat status {};
    if (::lstat(encodedPath.constData(), &status) != 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return true;
        }
        if (errorMessage) {
            *errorMessage = systemErrorText(
                tryx::DeviceManagerMessages::tr("Cannot inspect the PASE metrics configuration file path"),
                errorNumber);
        }
        return false;
    }
    if (S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        status.st_nlink == 1) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = tryx::DeviceManagerMessages::tr(
            "The PASE metrics configuration file path is unsafe");
    }
    return false;
}

PaseMetricsConfigStore::LoadResult PaseMetricsConfigStore::load() {
    deviceSerial_.clear();
    overlay_ = {};
    writesEnabled_ = true;
    LoadResult result;
    const auto reject =
        [this, &result](LoadStatus status, const QString &warning) {
            deviceSerial_.clear();
            overlay_ = {};
            writesEnabled_ = false;
            result.status = status;
            result.writesEnabled = false;
            result.warnings.append(warning);
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
            systemErrorText(
                tryx::DeviceManagerMessages::tr("Cannot open the PASE metrics configuration directory"),
                errorNumber));
    }
    struct stat directoryStatus {};
    if (::fstat(directoryDescriptor.get(), &directoryStatus) != 0) {
        const int errorNumber = errno;
        return reject(
            LoadStatus::ReadFailed,
            systemErrorText(
                tryx::DeviceManagerMessages::tr("Cannot inspect the PASE metrics configuration directory"),
                errorNumber));
    }
    if (!S_ISDIR(directoryStatus.st_mode) ||
        directoryStatus.st_uid != ::geteuid()) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            tryx::DeviceManagerMessages::tr("Ignoring unsafe PASE metrics configuration directory"));
    }

    const ScopedFileDescriptor configDescriptor(::openat(
        directoryDescriptor.get(), "pase-metrics.json",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (configDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        return reject(
            errorNumber == ELOOP || errorNumber == ENOTDIR
                ? LoadStatus::IgnoredUnsafe
                : LoadStatus::ReadFailed,
            systemErrorText(
                tryx::DeviceManagerMessages::tr("Cannot open the PASE metrics configuration"),
                errorNumber));
    }
    struct stat configStatus {};
    if (::fstat(configDescriptor.get(), &configStatus) != 0) {
        const int errorNumber = errno;
        return reject(
            LoadStatus::ReadFailed,
            systemErrorText(
                tryx::DeviceManagerMessages::tr("Cannot inspect the PASE metrics configuration"),
                errorNumber));
    }
    if (!S_ISREG(configStatus.st_mode) ||
        configStatus.st_uid != ::geteuid() || configStatus.st_nlink != 1 ||
        configStatus.st_size <= 0) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            tryx::DeviceManagerMessages::tr("Ignoring unsafe PASE metrics configuration"));
    }
    if (configStatus.st_size > kMaximumConfigBytes) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            tryx::DeviceManagerMessages::tr("Ignoring oversized PASE metrics configuration"));
    }

    QFile file;
    if (!file.open(configDescriptor.get(), QIODevice::ReadOnly,
                   QFileDevice::DontCloseHandle)) {
        return reject(LoadStatus::ReadFailed, file.errorString());
    }
    const QByteArray payload = file.read(kMaximumConfigBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return reject(LoadStatus::ReadFailed, file.errorString());
    }
    if (payload.size() > kMaximumConfigBytes || !file.atEnd()) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            tryx::DeviceManagerMessages::tr("Ignoring oversized PASE metrics configuration"));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            tryx::DeviceManagerMessages::tr("Ignoring malformed PASE metrics configuration"));
    }

    const QJsonObject root = document.object();
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    if (!versionValue.isDouble()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            tryx::DeviceManagerMessages::tr("Ignoring malformed PASE metrics configuration"));
    }
    const qint64 version = versionValue.toInteger(-1);
    if (version != 1 && version != kFormatVersion) {
        return reject(
            LoadStatus::UnsupportedVersion,
            tryx::DeviceManagerMessages::tr("Ignoring unsupported PASE metrics configuration"));
    }
    const QJsonValue enabledValue = root.value(QStringLiteral("enabled"));
    const QJsonValue serialValue =
        root.value(QStringLiteral("deviceSerial"));
    if (!enabledValue.isBool() || !enabledValue.toBool() ||
        !serialValue.isString() ||
        !deviceSerialIsValid(serialValue.toString())) {
        return reject(
            LoadStatus::IgnoredMalformed,
            tryx::DeviceManagerMessages::tr("Ignoring malformed PASE metrics configuration"));
    }

    PrinterProtocol::PaseOverlayConfig overlay;
    if (version == 1) {
        QJsonObject legacyArea;
        legacyArea.insert(QStringLiteral("metrics"),
                          root.value(QStringLiteral("metrics")));
        legacyArea.insert(QStringLiteral("badges"), QJsonArray{});
        legacyArea.insert(QStringLiteral("alignment"),
                          root.value(QStringLiteral("alignment")));
        legacyArea.insert(QStringLiteral("textColor"),
                          root.value(QStringLiteral("textColor")));
        legacyArea.insert(QStringLiteral("verticalPlacement"),
                          QStringLiteral("Top"));
        if (!parseArea(legacyArea, &overlay.left) ||
            overlay.left.metrics.isEmpty()) {
            return reject(
                LoadStatus::IgnoredMalformed,
                tryx::DeviceManagerMessages::tr(
                    "Ignoring invalid legacy PASE metrics configuration"));
        }
        result.migrated = true;
    } else {
        const QJsonValue dualModeValue =
            root.value(QStringLiteral("dualMode"));
        const QJsonValue waterfallModeValue =
            root.value(QStringLiteral("waterfallMode"));
        if (!dualModeValue.isBool() || !waterfallModeValue.isBool()) {
            return reject(
                LoadStatus::IgnoredMalformed,
                tryx::DeviceManagerMessages::tr("Ignoring malformed PASE metrics configuration"));
        }
        overlay.dualMode = dualModeValue.toBool();
        overlay.waterfallMode = waterfallModeValue.toBool();
        if (!parseArea(root.value(QStringLiteral("left")),
                       &overlay.left) ||
            !parseArea(root.value(QStringLiteral("right")),
                       &overlay.right) ||
            !paseOverlayHasContent(overlay)) {
            return reject(
                LoadStatus::IgnoredMalformed,
                tryx::DeviceManagerMessages::tr("Ignoring invalid PASE overlay configuration"));
        }
    }

    deviceSerial_ = normalizedDeviceSerial(serialValue.toString());
    overlay_ = canonicalOverlay(overlay);
    result.status = LoadStatus::Loaded;
    return result;
}

PaseMetricsConfigStore::MutationResult
PaseMetricsConfigStore::persist(
    const QString &deviceSerial,
    const PrinterProtocol::PaseOverlayConfig &sourceOverlay) {
    if (!writesEnabled_) {
        return failureResult(
            ErrorCode::WritesDisabled,
            tryx::DeviceManagerMessages::tr(
                "PASE metrics configuration writes are disabled because the persistent file was not accepted safely"));
    }
    const QString serial = normalizedDeviceSerial(deviceSerial);
    if (!deviceSerialIsValid(serial)) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Cannot persist PASE metrics without a valid device serial"));
    }
    const PrinterProtocol::PaseOverlayConfig overlay =
        canonicalOverlay(sourceOverlay);
    if (!overlayIsValid(overlay)) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Cannot persist an invalid PASE overlay configuration"));
    }

    QString pathError;
    if (!ensureDirectory(&pathError)) {
        return failureResult(ErrorCode::DirectoryUnavailable, pathError);
    }
    if (!destinationPathIsSafe(&pathError)) {
        writesEnabled_ = false;
        return failureResult(ErrorCode::UnsafePath, pathError);
    }

    const QByteArray payload = serializedPayload(serial, overlay);
    if (payload.isEmpty() || payload.size() > kMaximumConfigBytes) {
        return failureResult(
            ErrorCode::SizeLimitExceeded,
            tryx::DeviceManagerMessages::tr("The PASE metrics configuration exceeds its size limit"));
    }

    QSaveFile file(configPath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return failureResult(ErrorCode::WriteFailed, file.errorString());
    }
    if (file.write(payload) != payload.size()) {
        const QString detail = file.errorString().isEmpty()
            ? tryx::DeviceManagerMessages::tr("Cannot write the PASE metrics configuration")
            : file.errorString();
        file.cancelWriting();
        return failureResult(ErrorCode::WriteFailed, detail);
    }
    if (!file.commit()) {
        return failureResult(ErrorCode::CommitFailed, file.errorString());
    }

    deviceSerial_ = serial;
    overlay_ = overlay;
    return successResult();
}

PaseMetricsConfigStore::MutationResult
PaseMetricsConfigStore::clearForDevice(const QString &deviceSerial) {
    if (!writesEnabled_) {
        return failureResult(
            ErrorCode::WritesDisabled,
            tryx::DeviceManagerMessages::tr(
                "PASE metrics configuration writes are disabled because the persistent file was not accepted safely"));
    }
    const QString serial = normalizedDeviceSerial(deviceSerial);
    if (!deviceSerialIsValid(serial)) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Cannot clear PASE metrics without a valid device serial"));
    }
    if (!deviceSerial_.isEmpty() && deviceSerial_ != serial) {
        return successResult();
    }

    QString pathError;
    const QFileInfo info(configPath());
    if (deviceSerial_.isEmpty() && info.exists()) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr(
                "Cannot clear a PASE metrics configuration before its device identity is loaded"));
    }
    if (deviceSerial_.isEmpty()) {
        return successResult();
    }
    if (!ensureDirectory(&pathError)) {
        writesEnabled_ = false;
        return failureResult(ErrorCode::DirectoryUnavailable, pathError);
    }
    if (!destinationPathIsSafe(&pathError)) {
        writesEnabled_ = false;
        return failureResult(ErrorCode::UnsafePath, pathError);
    }
    if (info.exists() && !QFile::remove(configPath())) {
        return failureResult(
            ErrorCode::RemoveFailed,
            tryx::DeviceManagerMessages::tr("Cannot remove the previous PASE metrics configuration"));
    }

    deviceSerial_.clear();
    overlay_ = {};
    return successResult();
}

std::optional<PrinterProtocol::PaseOverlayConfig>
PaseMetricsConfigStore::overlayForDevice(
    const QString &deviceSerial) const {
    const QString serial = normalizedDeviceSerial(deviceSerial);
    if (!deviceSerialIsValid(serial) || serial != deviceSerial_) {
        return std::nullopt;
    }
    return overlay_;
}

}  // namespace tryx
