#include "devicemanager.h"
#include "printerprotocol.h"
#include "hudrenderer.h"
#include "systemmonitor.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QColor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <utility>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

// --- DeviceWorker ---

namespace {

constexpr int kMaxPrinterKeepaliveWriteRetries = 3;
constexpr int kPrinterKeepaliveRetryBackoffMs = 500;
constexpr int kMaxTerminalOperationHistory = 32;
constexpr int kRetryCacheFormatVersion = 9;
constexpr int kMediaCatalogFormatVersion = 2;
constexpr int kDeleteIntentFormatVersion = 1;
constexpr int kPaseMetricsConfigFormatVersion = 2;
constexpr qint64 kMaxRetryCacheBytes = 500LL * 1024LL * 1024LL;
constexpr qint64 kMaxThumbnailBytes = 16LL * 1024LL * 1024LL;
constexpr qint64 kMediaPreparationOutputCapBytes =
    kMaxRetryCacheBytes + 1024LL * 1024LL;
constexpr qint64 kMaxRetryManifestBytes = 256LL * 1024LL;
constexpr qint64 kMaxMediaCatalogIndexBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kMaxSourceMediaBytes = 8LL * 1024LL * 1024LL * 1024LL;
constexpr int kMediaPreparationDeadlineMs = 15 * 60 * 1000;
constexpr int kThumbnailPreparationDeadlineMs = 2 * 60 * 1000;
constexpr qint64 kFileTransmitChunkSize = 0x40000;

struct PrinterProcessClock {
    PrinterProcessClock() {
        timer.start();
    }

    QElapsedTimer timer;
};

qint64 printerMonotonicMilliseconds() {
    static const PrinterProcessClock clock;
    return clock.timer.elapsed();
}

QString printerStructuredValue(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString printerOverlayLeaseModeName(PrinterOverlayLeaseMode mode) {
    switch (mode) {
    case PrinterOverlayLeaseMode::PingAndOverlayLease:
        return QStringLiteral("ping-and-overlay-lease");
    case PrinterOverlayLeaseMode::PingOnly:
        return QStringLiteral("ping-only");
    }
    return QStringLiteral("unknown");
}

QString printerKeepaliveOutcomeName(
    PrinterProtocol::KeepaliveOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::KeepaliveOutcome::Sent:
        return QStringLiteral("sent");
    case PrinterProtocol::KeepaliveOutcome::RetryableFailure:
        return QStringLiteral("retryable-failure");
    case PrinterProtocol::KeepaliveOutcome::FatalFailure:
        return QStringLiteral("fatal-failure");
    }
    return QStringLiteral("unknown");
}

QString printerDiscoveryStateName(
    PrinterProtocol::DiscoveryState state) {
    switch (state) {
    case PrinterProtocol::DiscoveryState::Absent:
        return QStringLiteral("absent");
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
        return QStringLiteral("rockchip-gadget-391a-0006");
    case PrinterProtocol::DiscoveryState::Enumerating391a1021:
        return QStringLiteral("enumerating-391a-1021");
    case PrinterProtocol::DiscoveryState::Ready:
        return QStringLiteral("ready");
    case PrinterProtocol::DiscoveryState::PermissionDenied:
        return QStringLiteral("permission-denied");
    case PrinterProtocol::DiscoveryState::Ambiguous:
        return QStringLiteral("ambiguous");
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        return QStringLiteral("monitoring-unavailable");
    }
    return QStringLiteral("unknown");
}

void logPrinterLifecycleEvent(
    const QString &eventName, quint64 generation,
    std::initializer_list<QPair<QString, QString>> fields = {}) {
    QStringList parts{
        QStringLiteral("tryx_lifecycle"),
        QStringLiteral("event=") + printerStructuredValue(eventName),
        QStringLiteral("utc=") +
            printerStructuredValue(
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),
        QStringLiteral("monotonic_ms=") +
            QString::number(printerMonotonicMilliseconds()),
        QStringLiteral("generation=") + QString::number(generation)
    };
    for (const auto &field : fields) {
        parts.append(field.first + QLatin1Char('=') +
                     printerStructuredValue(field.second));
    }
    qInfo().noquote() << parts.join(QLatin1Char(' '));
}

QString generatedPrinterMediaName(const QString &extension) {
    const QString cleanExtension = extension.startsWith(QLatin1Char('.'))
        ? extension.mid(1).toLower()
        : extension.toLower();
    const QString nonce = QUuid::createUuid()
                              .toString(QUuid::WithoutBraces)
                              .left(8);
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") +
           QLatin1Char('-') + nonce + QLatin1Char('.') + cleanExtension;
}

QString printerTempPath(const QString &fileName) {
    const QString directory =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("prepared-media"));
    QDir().mkpath(directory);
    return QDir(directory).filePath(fileName);
}

QString sha256File(
    const QString &path,
    const std::function<bool()> &isCancelled = std::function<bool()>()) {
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

struct SafeSourceHashResult {
    QString sha256;
    qint64 size = 0;
    QString error;
    bool cancelled = false;
};

bool sameFileTimestamp(const timespec &left, const timespec &right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
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
        before.st_size <= 0 || before.st_size > kMaxSourceMediaBytes) {
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

bool isSha256Hex(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        if (!character.isDigit() &&
            (character < QLatin1Char('a') ||
             character > QLatin1Char('f'))) {
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
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString printerConversionProfile(const QString &path) {
    QString typeName;
    switch (panorama::Media::detect_type(path.toStdString())) {
    case panorama::MediaType::Image:
        typeName = QStringLiteral("image-60s");
        break;
    case panorama::MediaType::Video:
        typeName = QStringLiteral("video");
        break;
    case panorama::MediaType::Gif:
        typeName = QStringLiteral("gif");
        break;
    case panorama::MediaType::Unknown:
        return {};
    }
    return QStringLiteral(
               "pase-h264-v1-%1-2240x1080-yuv420p-30fps-libx264-veryfast-crf23")
        .arg(typeName);
}

QString mutationOutcomeName(PrinterProtocol::MutationOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::MutationOutcome::NotStarted:
        return QStringLiteral("NotStarted");
    case PrinterProtocol::MutationOutcome::Rejected:
        return QStringLiteral("Rejected");
    case PrinterProtocol::MutationOutcome::VerificationFailed:
        return QStringLiteral("VerificationFailed");
    case PrinterProtocol::MutationOutcome::Succeeded:
        return QStringLiteral("Succeeded");
    case PrinterProtocol::MutationOutcome::Cancelled:
        return QStringLiteral("Cancelled");
    case PrinterProtocol::MutationOutcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    case PrinterProtocol::MutationOutcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    }
    return QStringLiteral("PartialOrUnknown");
}

QString h264PrinterName(const QString &baseName) {
    return QStringLiteral("%1.h264_%2x%3")
        .arg(baseName)
        .arg(HudRenderer::DISPLAY_WIDTH)
        .arg(HudRenderer::DISPLAY_HEIGHT);
}

QString printerPresetMediaFile(const QString &presetId) {
    if (presetId == QStringLiteral("Pre-set 1: Cooling delivery")) {
        return QStringLiteral("default_01.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 2: Migration")) {
        return QStringLiteral("default_02.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 3: Quantum time capsule")) {
        return QStringLiteral("default_04.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 4: Exo-Ecologies")) {
        return QStringLiteral("default_03.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 5: Racing")) {
        return QStringLiteral("default_05.mp4.h264_2240x1080");
    }
    if (presetId == QStringLiteral("Pre-set 6: Shuttle")) {
        return QStringLiteral("default_06.mp4.h264_2240x1080");
    }
    return {};
}

QString printerMediaConfigName(const QString &mediaFile) {
    const QString trimmed = mediaFile.trimmed();
    if (trimmed.startsWith(QStringLiteral("/userdata/")) ||
        trimmed.startsWith(QStringLiteral("/sdcard/pcMedia/"))) {
        return QFileInfo(trimmed).fileName();
    }
    return trimmed;
}

bool isSupportedPaseMetricLabel(const QString &label) {
    static const QSet<QString> supported{
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("CPU Usage"),
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Frequency"),
        QStringLiteral("GPU Usage"),
        QStringLiteral("GPU Power"),
        QStringLiteral("Memory Frequency"),
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time"),
    };
    return supported.contains(label);
}

bool hasDuplicateMetricLabels(const QStringList &labels) {
    QSet<QString> seen;
    for (const QString &label : labels) {
        if (seen.contains(label)) {
            return true;
        }
        seen.insert(label);
    }
    return false;
}

bool isSupportedPaseBadge(const QString &badge) {
    return badge == QStringLiteral("CPU Badge") ||
           badge == QStringLiteral("GPU Badge");
}

bool hasDuplicateValues(const QStringList &values) {
    QSet<QString> seen;
    for (const QString &value : values) {
        if (seen.contains(value)) {
            return true;
        }
        seen.insert(value);
    }
    return false;
}

bool paseOverlayHasMetrics(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return !overlay.left.metrics.isEmpty() ||
           (overlay.dualMode && !overlay.right.metrics.isEmpty());
}

bool paseOverlayHasContent(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return paseOverlayHasMetrics(overlay) ||
           !overlay.left.badges.isEmpty() ||
           (overlay.dualMode && !overlay.right.badges.isEmpty());
}

QString normalizedPaseAlignment(const QString &alignment) {
    const QString trimmed = alignment.trimmed();
    return trimmed == QStringLiteral("Center") ||
                   trimmed == QStringLiteral("Right")
        ? trimmed
        : QStringLiteral("Left");
}

bool parsePaseTextColor(const QString &color, quint32 *value) {
    if (!value) {
        return false;
    }
    if (color.size() != 7 ||
        color.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (qsizetype index = 1; index < color.size(); ++index) {
        const QChar character = color.at(index);
        const bool hexadecimal =
            character.isDigit() ||
            (character >= QLatin1Char('a') &&
             character <= QLatin1Char('f')) ||
            (character >= QLatin1Char('A') &&
             character <= QLatin1Char('F'));
        if (!hexadecimal) {
            return false;
        }
    }
    bool ok = false;
    const quint32 parsed =
        color.mid(1).toUInt(&ok, 16);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

QString paseTextColorName(quint32 color) {
    return QColor::fromRgb(color & 0x00FFFFFFU)
        .name(QColor::HexRgb);
}

bool isValidPaseTextColor(const QString &color) {
    quint32 value = 0;
    return parsePaseTextColor(color, &value);
}

QString normalizedPaseVerticalPlacement(const QString &position) {
    return position.compare(QStringLiteral("Bottom"),
                            Qt::CaseInsensitive) == 0
        ? QStringLiteral("Bottom")
        : QStringLiteral("Top");
}

PrinterProtocol::PaseOverlayConfig paseOverlayFromApplyRequest(
    const TryxRuntimeApplyRequest &request) {
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = request.sysinfoLabels;
    overlay.left.badges = request.settingsBadges;
    overlay.left.alignment =
        normalizedPaseAlignment(request.settingsAlign);
    parsePaseTextColor(request.settingsColor,
                       &overlay.left.textColor);
    overlay.left.verticalPlacement =
        normalizedPaseVerticalPlacement(request.settingsPosition);
    overlay.dualMode =
        request.screenMode == QStringLiteral("Screen Splitting");
    overlay.waterfallMode =
        request.display.orientationPresent
        ? request.display.waterfallMode
        : request.waterfallMode;
    if (overlay.dualMode) {
        overlay.right.metrics = request.sysinfoLabels2;
        overlay.right.badges = request.settingsBadges2;
        overlay.right.alignment = normalizedPaseAlignment(
            request.settingsAlign2.isEmpty()
                ? request.settingsAlign
                : request.settingsAlign2);
        parsePaseTextColor(
            request.settingsColor2.isEmpty()
                ? request.settingsColor
                : request.settingsColor2,
            &overlay.right.textColor);
        overlay.right.verticalPlacement =
            normalizedPaseVerticalPlacement(
                request.settingsPosition2.isEmpty()
                    ? request.settingsPosition
                    : request.settingsPosition2);
    }
    return overlay;
}

PrinterProtocol::PaseOverlayConfig paseOverlayFromMetricsRequest(
    const TryxRuntimeMetricsConfigRequest &request) {
    PrinterProtocol::PaseOverlayConfig overlay;
    if (request.enabled) {
        overlay.left.metrics = request.metrics;
    }
    overlay.left.alignment = request.alignment;
    overlay.left.textColor = request.textColor;
    return overlay;
}

QJsonArray stringListToJson(const QStringList &values) {
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return array;
}

bool stringListFromJson(const QJsonObject &object, const QString &key,
                        QStringList *values) {
    if (!values || !object.value(key).isArray()) {
        return false;
    }
    QStringList parsed;
    const QJsonArray array = object.value(key).toArray();
    parsed.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isString()) {
            return false;
        }
        parsed.append(value.toString());
    }
    *values = parsed;
    return true;
}

QJsonObject runtimeApplyRequestToJson(
    const TryxRuntimeApplyRequest &request) {
    QJsonObject display;
    display.insert(QStringLiteral("brightnessPresent"),
                   request.display.brightnessPresent);
    display.insert(QStringLiteral("brightness"),
                   request.display.brightness);
    display.insert(QStringLiteral("standbyPresent"),
                   request.display.standbyPresent);
    display.insert(QStringLiteral("standbyEnabled"),
                   request.display.standbyEnabled);
    display.insert(QStringLiteral("backlightPresent"),
                   request.display.backlightPresent);
    display.insert(QStringLiteral("backlightEnabled"),
                   request.display.backlightEnabled);
    display.insert(QStringLiteral("orientationPresent"),
                   request.display.orientationPresent);
    display.insert(QStringLiteral("mirrorMode"),
                   request.display.mirrorMode);
    display.insert(QStringLiteral("waterfallMode"),
                   request.display.waterfallMode);

    QJsonObject object;
    object.insert(QStringLiteral("media"),
                  stringListToJson(request.media));
    object.insert(QStringLiteral("ratio"), request.ratio);
    object.insert(QStringLiteral("screenMode"), request.screenMode);
    object.insert(QStringLiteral("playMode"), request.playMode);
    object.insert(QStringLiteral("sysinfoLabels"),
                  stringListToJson(request.sysinfoLabels));
    object.insert(QStringLiteral("settingsPosition"),
                  request.settingsPosition);
    object.insert(QStringLiteral("settingsColor"),
                  request.settingsColor);
    object.insert(QStringLiteral("settingsAlign"),
                  request.settingsAlign);
    object.insert(QStringLiteral("settingsBadges"),
                  stringListToJson(request.settingsBadges));
    object.insert(QStringLiteral("filterOpacity"),
                  request.filterOpacity);
    object.insert(QStringLiteral("presetId"), request.presetId);
    object.insert(QStringLiteral("sysinfoLabels2"),
                  stringListToJson(request.sysinfoLabels2));
    object.insert(QStringLiteral("settingsBadges2"),
                  stringListToJson(request.settingsBadges2));
    object.insert(QStringLiteral("settingsPosition2"),
                  request.settingsPosition2);
    object.insert(QStringLiteral("settingsColor2"),
                  request.settingsColor2);
    object.insert(QStringLiteral("settingsAlign2"),
                  request.settingsAlign2);
    object.insert(QStringLiteral("waterfallMode"),
                  request.waterfallMode);
    object.insert(QStringLiteral("replaceOverlay"),
                  request.replaceOverlay);
    object.insert(QStringLiteral("display"), display);
    return object;
}

bool runtimeApplyRequestFromJson(
    const QJsonObject &object, TryxRuntimeApplyRequest *request,
    bool requireBacklightFields) {
    if (!request ||
        !object.value(QStringLiteral("ratio")).isString() ||
        !object.value(QStringLiteral("screenMode")).isString() ||
        !object.value(QStringLiteral("playMode")).isString() ||
        !object.value(QStringLiteral("settingsPosition")).isString() ||
        !object.value(QStringLiteral("settingsColor")).isString() ||
        !object.value(QStringLiteral("settingsAlign")).isString() ||
        !object.value(QStringLiteral("filterOpacity")).isDouble() ||
        !object.value(QStringLiteral("presetId")).isString() ||
        !object.value(QStringLiteral("settingsPosition2")).isString() ||
        !object.value(QStringLiteral("settingsColor2")).isString() ||
        !object.value(QStringLiteral("settingsAlign2")).isString() ||
        !object.value(QStringLiteral("waterfallMode")).isBool() ||
        !object.value(QStringLiteral("replaceOverlay")).isBool() ||
        !object.value(QStringLiteral("display")).isObject()) {
        return false;
    }

    TryxRuntimeApplyRequest parsed;
    if (!stringListFromJson(object, QStringLiteral("media"),
                            &parsed.media) ||
        !stringListFromJson(object, QStringLiteral("sysinfoLabels"),
                            &parsed.sysinfoLabels) ||
        !stringListFromJson(object, QStringLiteral("settingsBadges"),
                            &parsed.settingsBadges) ||
        !stringListFromJson(object, QStringLiteral("sysinfoLabels2"),
                            &parsed.sysinfoLabels2) ||
        !stringListFromJson(object, QStringLiteral("settingsBadges2"),
                            &parsed.settingsBadges2)) {
        return false;
    }
    parsed.ratio = object.value(QStringLiteral("ratio")).toString();
    parsed.screenMode =
        object.value(QStringLiteral("screenMode")).toString();
    parsed.playMode =
        object.value(QStringLiteral("playMode")).toString();
    parsed.settingsPosition =
        object.value(QStringLiteral("settingsPosition")).toString();
    parsed.settingsColor =
        object.value(QStringLiteral("settingsColor")).toString();
    parsed.settingsAlign =
        object.value(QStringLiteral("settingsAlign")).toString();
    parsed.filterOpacity =
        object.value(QStringLiteral("filterOpacity")).toInt();
    parsed.presetId =
        object.value(QStringLiteral("presetId")).toString();
    parsed.settingsPosition2 =
        object.value(QStringLiteral("settingsPosition2")).toString();
    parsed.settingsColor2 =
        object.value(QStringLiteral("settingsColor2")).toString();
    parsed.settingsAlign2 =
        object.value(QStringLiteral("settingsAlign2")).toString();
    parsed.waterfallMode =
        object.value(QStringLiteral("waterfallMode")).toBool();
    parsed.replaceOverlay =
        object.value(QStringLiteral("replaceOverlay")).toBool();

    const QJsonObject display =
        object.value(QStringLiteral("display")).toObject();
    if (!display.value(QStringLiteral("brightnessPresent")).isBool() ||
        !display.value(QStringLiteral("brightness")).isDouble() ||
        !display.value(QStringLiteral("standbyPresent")).isBool() ||
        !display.value(QStringLiteral("standbyEnabled")).isBool() ||
        !display.value(QStringLiteral("orientationPresent")).isBool() ||
        !display.value(QStringLiteral("mirrorMode")).isBool() ||
        !display.value(QStringLiteral("waterfallMode")).isBool()) {
        return false;
    }
    const QJsonValue backlightPresent =
        display.value(QStringLiteral("backlightPresent"));
    const QJsonValue backlightEnabled =
        display.value(QStringLiteral("backlightEnabled"));
    if ((requireBacklightFields &&
         (!backlightPresent.isBool() ||
          !backlightEnabled.isBool())) ||
        (!backlightPresent.isUndefined() &&
         !backlightPresent.isBool()) ||
        (!backlightEnabled.isUndefined() &&
         !backlightEnabled.isBool())) {
        return false;
    }
    parsed.display.brightnessPresent =
        display.value(QStringLiteral("brightnessPresent")).toBool();
    parsed.display.brightness =
        display.value(QStringLiteral("brightness")).toInt();
    parsed.display.standbyPresent =
        display.value(QStringLiteral("standbyPresent")).toBool();
    parsed.display.standbyEnabled =
        display.value(QStringLiteral("standbyEnabled")).toBool();
    parsed.display.backlightPresent =
        backlightPresent.toBool(false);
    parsed.display.backlightEnabled =
        backlightEnabled.toBool(true);
    parsed.display.orientationPresent =
        display.value(QStringLiteral("orientationPresent")).toBool();
    parsed.display.mirrorMode =
        display.value(QStringLiteral("mirrorMode")).toBool();
    parsed.display.waterfallMode =
        display.value(QStringLiteral("waterfallMode")).toBool();
    if (parsed.settingsColor.isEmpty()) {
        parsed.settingsColor =
            QStringLiteral("#dcdcdc");
    }
    if (parsed.settingsColor2.isEmpty()) {
        parsed.settingsColor2 =
            parsed.settingsColor;
    }
    *request = parsed;
    return true;
}

bool paseUploadApplyRequestIsValid(
    const TryxRuntimeApplyRequest &request) {
    const auto metricsAreValid = [](const QStringList &metrics) {
        return metrics.size() <= 3 &&
               !hasDuplicateMetricLabels(metrics) &&
               std::all_of(
                   metrics.cbegin(), metrics.cend(),
                   [](const QString &label) {
                       return isSupportedPaseMetricLabel(label);
                   });
    };
    const auto badgesAreValid = [](const QStringList &badges) {
        return badges.size() <= 2 &&
               !hasDuplicateValues(badges) &&
               std::all_of(
                   badges.cbegin(), badges.cend(),
                   [](const QString &badge) {
                       return isSupportedPaseBadge(badge);
                   });
    };
    return request.media.isEmpty() &&
           request.screenMode == QStringLiteral("Full Screen") &&
           (request.playMode == QStringLiteral("Single") ||
            request.playMode == QStringLiteral("Loop") ||
            request.playMode == QStringLiteral("Shuffle")) &&
           request.ratio == QStringLiteral("2:1") &&
           metricsAreValid(request.sysinfoLabels) &&
           badgesAreValid(request.settingsBadges) &&
           isValidPaseTextColor(request.settingsColor) &&
           request.sysinfoLabels2.isEmpty() &&
           request.settingsBadges2.isEmpty() &&
           !request.display.standbyPresent &&
           (!request.display.brightnessPresent ||
            (request.display.brightness >= 0 &&
             request.display.brightness <= 100));
}

bool writeJsonObjectAtomically(const QString &path,
                               const QJsonObject &object,
                               QString *errorMessage) {
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Cannot create the state directory: %1").arg(directory);
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray payload =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

void collectPaseMetricValues(SystemMonitor *monitor, QStringList *labels,
                             QStringList *values, QStringList *units) {
    labels->clear();
    values->clear();
    units->clear();
    monitor->update();
    const SystemMetrics metrics = monitor->currentMetrics();
    const auto appendMetric = [labels, values, units](
                                  const QString &label, double value,
                                  const QString &unit, bool available,
                                  int precision = 0) {
        if (!available) {
            return;
        }
        labels->append(label);
        values->append(QString::number(value, 'f', precision));
        units->append(unit);
    };
    appendMetric(QStringLiteral("CPU Temperature"), metrics.cpu.temperature,
                 QStringLiteral("°C"), metrics.cpu.temperatureAvailable);
    appendMetric(QStringLiteral("CPU Frequency"), metrics.cpu.frequencyMHz,
                 QStringLiteral("MHZ"), metrics.cpu.frequencyAvailable);
    appendMetric(QStringLiteral("CPU Usage"), metrics.cpu.usagePercent,
                 QStringLiteral("%"), metrics.cpu.usageAvailable);
    appendMetric(QStringLiteral("CPU Power"), metrics.cpu.powerWatts,
                 QStringLiteral("W"), metrics.cpu.powerAvailable, 1);
    if (!metrics.gpus.isEmpty()) {
        const GpuMetrics &gpu = metrics.gpus.constFirst();
        appendMetric(QStringLiteral("GPU Temperature"), gpu.temperature,
                     QStringLiteral("°C"), gpu.temperatureAvailable);
        appendMetric(QStringLiteral("GPU Frequency"), gpu.frequencyMHz,
                     QStringLiteral("MHZ"), gpu.frequencyAvailable);
        appendMetric(QStringLiteral("GPU Usage"), gpu.usagePercent,
                     QStringLiteral("%"), gpu.usageAvailable);
        appendMetric(QStringLiteral("GPU Power"), gpu.powerWatts,
                     QStringLiteral("W"), gpu.powerAvailable, 1);
    }
    appendMetric(QStringLiteral("Memory Frequency"),
                 metrics.ram.frequencyMHz, QStringLiteral("MHZ"),
                 metrics.ram.frequencyAvailable);
    appendMetric(QStringLiteral("Memory Usage"), metrics.ram.usagePercent,
                 QStringLiteral("%"), metrics.ram.usageAvailable);
}

QString paseCpuBadgeText() {
    const QString model = SystemMonitor::cpuModelName().trimmed();
    return model.isEmpty() ? QStringLiteral("CPU") : model;
}

QString paseGpuBadgeText(SystemMonitor *monitor) {
    if (!monitor) {
        return QStringLiteral("GPU");
    }
    const QString model = monitor->primaryGpuModelName().trimmed();
    return model.isEmpty() ? QStringLiteral("GPU") : model;
}

bool paseOverlayRequestsBadge(
    const PrinterProtocol::PaseOverlayConfig &overlay,
    const QString &badge) {
    return overlay.left.badges.contains(badge) ||
           (overlay.dualMode &&
            overlay.right.badges.contains(badge));
}

void hydratePaseBadgeText(
    PrinterProtocol::PaseOverlayConfig *overlay,
    SystemMonitor *monitor) {
    if (!overlay) {
        return;
    }
    const bool cpuRequested = paseOverlayRequestsBadge(
        *overlay, QStringLiteral("CPU Badge"));
    const bool gpuRequested = paseOverlayRequestsBadge(
        *overlay, QStringLiteral("GPU Badge"));
    overlay->cpuBadgeText =
        cpuRequested ? paseCpuBadgeText() : QString();
    overlay->gpuBadgeText =
        gpuRequested ? paseGpuBadgeText(monitor) : QString();
    if (cpuRequested || gpuRequested) {
        qInfo().noquote()
            << QStringLiteral(
                   "Hydrated PASE badge models: CPU=%1 GPU=%2")
                   .arg(
                       cpuRequested
                           ? overlay->cpuBadgeText
                           : QStringLiteral("<not requested>"),
                       gpuRequested
                           ? overlay->gpuBadgeText
                           : QStringLiteral("<not requested>"));
    }
}

}  // namespace

// --- PrinterMediaPreparer ---

PrinterMediaPreparer::PrinterMediaPreparer(QObject *parent)
    : QObject(parent),
      process_(new QProcess(this)),
      processDeadlineTimer_(new QTimer(this)) {
    processDeadlineTimer_->setSingleShot(true);
    connect(processDeadlineTimer_, &QTimer::timeout, this, [this]() {
        if (!active_ || process_->state() == QProcess::NotRunning) {
            return;
        }
        preparationTimedOut_ = true;
        process_->kill();
    });
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, [this]() {
        processOutput_.append(process_->readAll());
        constexpr qsizetype kMaxDiagnosticBytes = 8192;
        if (processOutput_.size() > kMaxDiagnosticBytes) {
            processOutput_ = processOutput_.right(kMaxDiagnosticBytes);
        }
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                finishPreparation(exitCode, status == QProcess::NormalExit);
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (active_ && error == QProcess::FailedToStart) {
                    finishPreparation(-1, false);
                }
            });
}

PrinterMediaPreparer::~PrinterMediaPreparer() {
    shutdown();
}

void PrinterMediaPreparer::analyzeSource(
    const QString &operationId, const QString &localPath,
    quint64 generation) {
    const auto isCancelled = [this, operationId, generation]() {
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != generation) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(operationId);
    };
    const QString profile = printerConversionProfile(localPath);
    if (profile.isEmpty()) {
        emit failed(operationId, tr("Unsupported media file type"),
                    generation);
        return;
    }
    const SafeSourceHashResult result =
        hashRegularSourceFile(localPath, isCancelled);
    if (result.cancelled) {
        return;
    }
    if (!isSha256Hex(result.sha256) || result.size <= 0) {
        emit failed(
            operationId,
            result.error.isEmpty()
                ? tr("Could not calculate the source media content hash")
                : result.error,
            generation);
        return;
    }
    emit sourceAnalyzed(operationId, localPath, result.sha256,
                        result.size, profile, generation);
}

void PrinterMediaPreparer::cancelRetryValidation(
    const QString &validationId) {
    if (validationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&retryValidationMutex_);
    cancelledRetryValidations_.insert(validationId);
}

void PrinterMediaPreparer::clearRetryValidationCancellation(
    const QString &validationId) {
    QMutexLocker locker(&retryValidationMutex_);
    cancelledRetryValidations_.remove(validationId);
}

void PrinterMediaPreparer::requestOperationCancellation(
    const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&preparationCancellationMutex_);
    cancelledPreparationOperations_.insert(operationId);
}

void PrinterMediaPreparer::requestGenerationCancellation(
    quint64 currentGeneration) {
    quint64 observed = preparationGenerationGate_.load(
        std::memory_order_acquire);
    while (observed < currentGeneration &&
           !preparationGenerationGate_.compare_exchange_weak(
               observed, currentGeneration, std::memory_order_release,
               std::memory_order_acquire)) {
    }
}

void PrinterMediaPreparer::prepare(const QString &operationId,
                                   const QString &devicePath,
                                   const QString &localPath,
                                   const QString &expectedSourceSha256,
                                   quint64 generation) {
    if (shuttingDown_) {
        return;
    }
    if (active_) {
        pendingOperationId_ = operationId;
        pendingDevicePath_ = devicePath;
        pendingLocalPath_ = localPath;
        pendingExpectedSourceSha256_ = expectedSourceSha256;
        pendingGeneration_ = generation;
        hasPending_ = true;
        cancelling_ = true;
        process_->kill();
        return;
    }
    startPreparation(operationId, devicePath, localPath,
                     expectedSourceSha256, generation);
}

void PrinterMediaPreparer::startPreparation(const QString &operationId,
                                            const QString &devicePath,
                                            const QString &localPath,
                                            const QString &expectedSourceSha256,
                                            quint64 generation) {
    const auto isCancelled = [this, operationId, generation]() {
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != generation) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(operationId);
    };
    if (isCancelled()) {
        {
            QMutexLocker locker(&preparationCancellationMutex_);
            cancelledPreparationOperations_.remove(operationId);
        }
        emit failed(operationId,
                    tr("Media preparation was cancelled before it started"),
                    generation);
        return;
    }
    const QFileInfo inputInfo(localPath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        emit failed(operationId, tr("Media file does not exist"), generation);
        return;
    }
    if (!expectedSourceSha256.isEmpty() &&
        !isSha256Hex(expectedSourceSha256)) {
        emit failed(operationId,
                    tr("Source media content identity is invalid"),
                    generation);
        return;
    }

    const auto type = panorama::Media::detect_type(localPath.toStdString());
    QString baseExtension;
    switch (type) {
    case panorama::MediaType::Image:
        baseExtension = QStringLiteral("png");
        break;
    case panorama::MediaType::Video:
        baseExtension = QStringLiteral("mp4");
        break;
    case panorama::MediaType::Gif:
        baseExtension = QStringLiteral("gif");
        break;
    case panorama::MediaType::Unknown:
        emit failed(operationId, tr("Unsupported media file type"), generation);
        return;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        emit failed(operationId,
                    tr("ffmpeg not found. Install it with your system package manager"),
                    generation);
        return;
    }

    const QString remoteBaseName = generatedPrinterMediaName(baseExtension);
    const QString remoteName = h264PrinterName(remoteBaseName);
    const QString outputPath = printerTempPath(remoteName);
    const QString stagedThumbnailPath = outputPath + QStringLiteral(".jpg");
    const QString stagedThumbnailTempPath =
        outputPath + QStringLiteral(".part.jpg");
    QFile::remove(outputPath);
    QFile::remove(stagedThumbnailPath);
    QFile::remove(stagedThumbnailTempPath);

    QStringList arguments{QStringLiteral("-y")};
    if (type == panorama::MediaType::Image) {
        arguments << QStringLiteral("-loop") << QStringLiteral("1")
                  << QStringLiteral("-framerate") << QStringLiteral("30")
                  << QStringLiteral("-t") << QStringLiteral("60");
    }
    const QString width = QString::number(HudRenderer::DISPLAY_WIDTH);
    const QString height = QString::number(HudRenderer::DISPLAY_HEIGHT);
    const QString filter = QStringLiteral(
        "scale=%1:%2:force_original_aspect_ratio=decrease,"
        "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black,"
        "setsar=1,format=yuv420p,fps=30").arg(width, height);
    arguments << QStringLiteral("-i") << localPath
              << QStringLiteral("-c:v") << QStringLiteral("libx264")
              << QStringLiteral("-preset") << QStringLiteral("veryfast")
              << QStringLiteral("-crf") << QStringLiteral("23")
              << QStringLiteral("-vf") << filter
              << QStringLiteral("-an")
              << QStringLiteral("-f") << QStringLiteral("h264")
              << QStringLiteral("-fs")
              << QString::number(kMediaPreparationOutputCapBytes)
              << outputPath;

    operationId_ = operationId;
    devicePath_ = devicePath;
    sourcePath_ = localPath;
    uploadPath_ = outputPath;
    remoteName_ = remoteName;
    stagedThumbnailTempPath_ = stagedThumbnailTempPath;
    stagedThumbnailPath_ = stagedThumbnailPath;
    preparedSha256_.clear();
    expectedSourceSha256_ = expectedSourceSha256;
    generation_ = generation;
    processOutput_.clear();
    cancelling_ = false;
    preparationTimedOut_ = false;
    mediaPreparationDeadline_ =
        QDeadlineTimer(kMediaPreparationDeadlineMs);
    active_ = true;
    phase_ = PreparationPhase::Media;
    emit progress(operationId,
                  tr("Converting to printer-class H264..."), generation);
    process_->setProgram(ffmpeg);
    process_->setArguments(arguments);
    process_->start();
    processDeadlineTimer_->start(kMediaPreparationDeadlineMs);
}

void PrinterMediaPreparer::finishPreparation(int exitCode, bool normalExit) {
    if (!active_) {
        return;
    }
    processDeadlineTimer_->stop();
    processOutput_.append(process_->readAll());
    if (phase_ == PreparationPhase::Media) {
        finishMediaPreparation(exitCode, normalExit);
    } else if (phase_ == PreparationPhase::Thumbnail) {
        finishThumbnailPreparation(exitCode, normalExit);
    }
}

void PrinterMediaPreparer::finishMediaPreparation(int exitCode,
                                                  bool normalExit) {
    if (preparationTimedOut_) {
        failPreparation(tr("Conversion to printer-class H264 timed out"),
                        false);
        return;
    }
    const bool cancelled = cancelling_ || shuttingDown_;
    const QFileInfo outputInfo(uploadPath_);
    if (!cancelled && outputInfo.exists() && outputInfo.isFile() &&
        outputInfo.size() > kMaxRetryCacheBytes) {
        failPreparation(
            tr("Prepared H264 exceeds the supported upload size"), false);
        return;
    }
    const bool outputReady = !cancelled && normalExit && exitCode == 0 &&
                             outputInfo.exists() && outputInfo.isFile() &&
                             outputInfo.size() > 0;
    if (!outputReady) {
        QString detail = QString::fromLocal8Bit(processOutput_).trimmed();
        if (detail.size() > 1000) {
            detail = detail.right(1000);
        }
        failPreparation(
            detail.isEmpty()
                ? tr("Conversion to printer-class H264 failed")
                : tr("Conversion to printer-class H264 failed: %1").arg(detail),
            cancelled);
        return;
    }

    const QString activeOperationId = operationId_;
    const quint64 activeGeneration = generation_;
    const auto hashCancelled = [this, activeOperationId,
                                activeGeneration]() {
        if (mediaPreparationDeadline_.hasExpired()) {
            return true;
        }
        const quint64 gate = preparationGenerationGate_.load(
            std::memory_order_acquire);
        if (gate != 0 && gate != activeGeneration) {
            return true;
        }
        QMutexLocker locker(&preparationCancellationMutex_);
        return cancelledPreparationOperations_.contains(activeOperationId);
    };
    preparedSha256_ = sha256File(uploadPath_, hashCancelled);
    if (preparedSha256_.isEmpty()) {
        if (mediaPreparationDeadline_.hasExpired()) {
            failPreparation(
                tr("Conversion to printer-class H264 timed out"), false);
            return;
        }
        if (hashCancelled()) {
            failPreparation(QString(), true);
            return;
        }
        failPreparation(tr("Could not verify the prepared H264 file"), false);
        return;
    }

    if (!expectedSourceSha256_.isEmpty()) {
        const SafeSourceHashResult sourceResult =
            hashRegularSourceFile(sourcePath_, hashCancelled);
        if (sourceResult.cancelled) {
            failPreparation(QString(), true);
            return;
        }
        if (!isSha256Hex(sourceResult.sha256) ||
            sourceResult.sha256 != expectedSourceSha256_) {
            failPreparation(
                sourceResult.error.isEmpty()
                    ? tr("Source media changed after content analysis")
                    : sourceResult.error,
                false);
            return;
        }
    }

    processOutput_.clear();
    preparationTimedOut_ = false;
    phase_ = PreparationPhase::Thumbnail;
    emit progress(operationId_, tr("Preparing a persistent preview..."),
                  generation_);
    process_->setProgram(
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
    process_->setArguments(
        {QStringLiteral("-y"), QStringLiteral("-f"),
         QStringLiteral("h264"), QStringLiteral("-framerate"),
         QStringLiteral("30"), QStringLiteral("-i"), uploadPath_,
         QStringLiteral("-vf"), QStringLiteral("scale=384:-2"),
         QStringLiteral("-frames:v"), QStringLiteral("1"),
         QStringLiteral("-q:v"), QStringLiteral("4"),
         stagedThumbnailTempPath_});
    process_->start();
    processDeadlineTimer_->start(kThumbnailPreparationDeadlineMs);
}

void PrinterMediaPreparer::finishThumbnailPreparation(int exitCode,
                                                      bool normalExit) {
    if (preparationTimedOut_) {
        QFile::remove(stagedThumbnailTempPath_);
        QFile::remove(stagedThumbnailPath_);
        emit progress(
            operationId_,
            tr("Persistent preview timed out; continuing with a placeholder"),
            generation_);
        completePreparation(QString());
        return;
    }
    const bool cancelled = cancelling_ || shuttingDown_;
    if (cancelled) {
        failPreparation(QString(), true);
        return;
    }

    QString thumbnailSha256;
    const QFileInfo thumbnailInfo(stagedThumbnailTempPath_);
    if (normalExit && exitCode == 0 && thumbnailInfo.exists() &&
        thumbnailInfo.isFile() && thumbnailInfo.size() > 0) {
        QImageReader reader(stagedThumbnailTempPath_);
        if (reader.canRead()) {
            thumbnailSha256 = sha256File(stagedThumbnailTempPath_);
        }
    }
    if (thumbnailSha256.isEmpty() ||
        !QFile::rename(stagedThumbnailTempPath_, stagedThumbnailPath_)) {
        thumbnailSha256.clear();
        QFile::remove(stagedThumbnailTempPath_);
        QFile::remove(stagedThumbnailPath_);
    }
    completePreparation(thumbnailSha256);
}

void PrinterMediaPreparer::completePreparation(
    const QString &thumbnailSha256) {
    const QString operationId = operationId_;
    const QString devicePath = devicePath_;
    const QString sourcePath = sourcePath_;
    const QString outputPath = uploadPath_;
    const QString remoteName = remoteName_;
    const QString preparedSha256 = preparedSha256_;
    const QString thumbnailPath = thumbnailSha256.isEmpty()
        ? QString()
        : stagedThumbnailPath_;
    const quint64 generation = generation_;

    deliveredPaths_.insert(outputPath);
    if (!thumbnailPath.isEmpty()) {
        deliveredPaths_.insert(thumbnailPath);
    }
    resetPreparationState();
    emit prepared(operationId, devicePath, sourcePath, outputPath,
                  remoteName, preparedSha256, thumbnailPath,
                  thumbnailSha256, generation);
    startPendingIfAvailable();
}

void PrinterMediaPreparer::failPreparation(const QString &message,
                                           bool cancelled) {
    const QString operationId = operationId_;
    const QString outputPath = uploadPath_;
    const QString thumbnailPath = stagedThumbnailPath_;
    const quint64 generation = generation_;
    if (!outputPath.isEmpty()) {
        QFile::remove(outputPath);
    }
    if (!stagedThumbnailTempPath_.isEmpty()) {
        QFile::remove(stagedThumbnailTempPath_);
    }
    if (!thumbnailPath.isEmpty()) {
        QFile::remove(thumbnailPath);
    }
    resetPreparationState();
    if (!cancelled) {
        emit failed(operationId, message, generation);
    }
    startPendingIfAvailable();
}

void PrinterMediaPreparer::resetPreparationState() {
    const QString completedOperationId = operationId_;
    processDeadlineTimer_->stop();
    active_ = false;
    cancelling_ = false;
    preparationTimedOut_ = false;
    mediaPreparationDeadline_ = QDeadlineTimer();
    phase_ = PreparationPhase::Idle;
    operationId_.clear();
    devicePath_.clear();
    sourcePath_.clear();
    uploadPath_.clear();
    remoteName_.clear();
    stagedThumbnailTempPath_.clear();
    stagedThumbnailPath_.clear();
    preparedSha256_.clear();
    expectedSourceSha256_.clear();
    generation_ = 0;
    processOutput_.clear();
    if (!completedOperationId.isEmpty()) {
        QMutexLocker locker(&preparationCancellationMutex_);
        cancelledPreparationOperations_.remove(completedOperationId);
    }
}

void PrinterMediaPreparer::startPendingIfAvailable() {
    if (!hasPending_ || shuttingDown_) {
        hasPending_ = false;
        return;
    }
    const QString operationId = pendingOperationId_;
    const QString devicePath = pendingDevicePath_;
    const QString localPath = pendingLocalPath_;
    const QString expectedSourceSha256 =
        pendingExpectedSourceSha256_;
    const quint64 generation = pendingGeneration_;
    hasPending_ = false;
    pendingOperationId_.clear();
    pendingDevicePath_.clear();
    pendingLocalPath_.clear();
    pendingExpectedSourceSha256_.clear();
    pendingGeneration_ = 0;
    startPreparation(operationId, devicePath, localPath,
                     expectedSourceSha256, generation);
}

void PrinterMediaPreparer::cancelStale(quint64 currentGeneration) {
    requestGenerationCancellation(currentGeneration);
    if (hasPending_ && pendingGeneration_ != currentGeneration) {
        hasPending_ = false;
        pendingOperationId_.clear();
        pendingDevicePath_.clear();
        pendingLocalPath_.clear();
        pendingExpectedSourceSha256_.clear();
        pendingGeneration_ = 0;
    }
    if (active_ && generation_ != currentGeneration) {
        cancelling_ = true;
        process_->kill();
    }
}

void PrinterMediaPreparer::cancelOperation(const QString &operationId) {
    requestOperationCancellation(operationId);
    bool operationRemovedBeforeStart = false;
    if (hasPending_ && pendingOperationId_ == operationId) {
        hasPending_ = false;
        pendingOperationId_.clear();
        pendingDevicePath_.clear();
        pendingLocalPath_.clear();
        pendingExpectedSourceSha256_.clear();
        pendingGeneration_ = 0;
        operationRemovedBeforeStart = true;
    }
    if (active_ && operationId_ == operationId) {
        cancelling_ = true;
        process_->kill();
        return;
    }
    if (operationRemovedBeforeStart || !active_ ||
        operationId_ != operationId) {
        QMutexLocker locker(&preparationCancellationMutex_);
        cancelledPreparationOperations_.remove(operationId);
    }
}

void PrinterMediaPreparer::validateRetryCache(
    const QString &validationId, const QString &preparedPath,
    const QString &expectedSha256) {
    const auto isCancelled = [this, validationId]() {
        QMutexLocker locker(&retryValidationMutex_);
        return cancelledRetryValidations_.contains(validationId);
    };
    if (isCancelled()) {
        emit retryCacheValidated(
            validationId, false, true,
            tr("Prepared-media validation was cancelled"));
        return;
    }
    const QFileInfo info(preparedPath);
    if (validationId.isEmpty() || !info.exists() || !info.isFile() ||
        info.isSymLink() ||
        info.size() <= 0 || !isSha256Hex(expectedSha256)) {
        emit retryCacheValidated(
            validationId, false, false,
            tr("Prepared media failed retry-cache validation"));
        return;
    }
    const QString actualSha256 = sha256File(preparedPath, isCancelled);
    if (isCancelled()) {
        emit retryCacheValidated(
            validationId, false, true,
            tr("Prepared-media validation was cancelled"));
        return;
    }
    if (actualSha256.isEmpty() || actualSha256 != expectedSha256) {
        emit retryCacheValidated(
            validationId, false, false,
            tr("Prepared media hash does not match the retry cache"));
        return;
    }
    emit retryCacheValidated(validationId, true, false, QString());
}

void PrinterMediaPreparer::releasePreparedFile(const QString &uploadPath) {
    deliveredPaths_.remove(uploadPath);
}

void PrinterMediaPreparer::shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    processDeadlineTimer_->stop();
    hasPending_ = false;
    pendingOperationId_.clear();
    pendingDevicePath_.clear();
    pendingLocalPath_.clear();
    pendingExpectedSourceSha256_.clear();
    pendingGeneration_ = 0;
    if (active_) {
        cancelling_ = true;
        process_->kill();
        process_->waitForFinished(3000);
        if (active_ && process_->state() == QProcess::NotRunning) {
            finishPreparation(process_->exitCode(),
                              process_->exitStatus() == QProcess::NormalExit);
        }
        if (active_) {
            QFile::remove(uploadPath_);
            QFile::remove(stagedThumbnailTempPath_);
            QFile::remove(stagedThumbnailPath_);
            resetPreparationState();
        }
    }
    for (const QString &path : std::as_const(deliveredPaths_)) {
        QFile::remove(path);
    }
    deliveredPaths_.clear();
}

// --- DeviceWorker ---

DeviceWorker::DeviceWorker(QObject *parent)
    : QObject(parent),
      printerProtocol_(std::make_unique<PrinterProtocol>()),
      printerKeepaliveTimer_(new QTimer(this)),
      printerMetricsTimer_(new QTimer(this)),
      printerRecoveryTimer_(new QTimer(this)),
      printerSystemMonitor_(new SystemMonitor(this)),
      printerCancellationFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      printerOperationCancellationFd_(
          eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    printerKeepaliveTimer_->setSingleShot(true);
    printerKeepaliveTimer_->setInterval(2000);
    connect(printerKeepaliveTimer_, &QTimer::timeout,
            this, &DeviceWorker::sendPrinterKeepalive);
    printerMetricsTimer_->setInterval(1000);
    connect(printerMetricsTimer_, &QTimer::timeout,
            this, &DeviceWorker::sendPrinterMetrics);
    printerRecoveryTimer_->setSingleShot(true);
    connect(printerRecoveryTimer_, &QTimer::timeout,
            this, &DeviceWorker::retryPrinterSessionStart);
}

DeviceWorker::~DeviceWorker() {
    stopPrinterSession();
    if (printerCancellationFd_ >= 0) {
        ::close(printerCancellationFd_);
        printerCancellationFd_ = -1;
    }
    if (printerOperationCancellationFd_ >= 0) {
        ::close(printerOperationCancellationFd_);
        printerOperationCancellationFd_ = -1;
    }
    if (device_ && device_->is_connected()) {
        device_->disconnect();
    }
}

void DeviceWorker::updatePrinterGenerationGate(quint64 generation,
                                                bool endpointReady) {
    printerGenerationGate_.store(generation, std::memory_order_release);
    printerEndpointReady_.store(endpointReady, std::memory_order_release);
    if (printerCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored = ::write(printerCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
    if (printerOperationCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored =
            ::write(printerOperationCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
}

void DeviceWorker::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerOverlayLeaseMode_ = mode;
    if (mode == PrinterOverlayLeaseMode::PingOnly) {
        printerOverlayLeaseRefreshNext_ = false;
    }
}

void DeviceWorker::cancelPrinterOperation(const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&printerOperationCancellationMutex_);
        cancelledPrinterOperationIds_.insert(operationId);
    }
    if (printerOperationCancellationFd_ < 0) {
        return;
    }
    const uint64_t value = 1;
    const ssize_t ignored =
        ::write(printerOperationCancellationFd_, &value, sizeof(value));
    Q_UNUSED(ignored);
}

void DeviceWorker::clearPrinterOperationCancellation(
    const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&printerOperationCancellationMutex_);
    cancelledPrinterOperationIds_.remove(operationId);
}

#ifdef TRYX_PROTOCOL_TESTING
void DeviceWorker::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    stopPrinterSession();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerProtocol_->adoptFileDescriptorForTesting(fd, devicePath);
}

bool DeviceWorker::printerSessionActiveForTesting() const {
    return printerSessionState_ == PrinterSessionState::Active;
}
#endif

void DeviceWorker::connectDevice(const QString &port) {
    std::string portStr;

    if (port.isEmpty()) {
        auto detected = panorama::Device::find_device();
        if (!detected) {
            emit error(tr("Device not found. Check the USB connection."));
            return;
        }
        portStr = *detected;
    } else {
        portStr = port.toStdString();
    }

    device_ = std::make_unique<panorama::Device>(portStr);
    if (!device_->connect()) {
        emit error(tr("Failed to connect to %1").arg(QString::fromStdString(portStr)));
        device_.reset();
        return;
    }

    doHandshake();
}

void DeviceWorker::disconnectDevice() {
    if (!device_) {
        return;
    }
    device_->disconnect();
    device_.reset();
    emit disconnected();
}

void DeviceWorker::doHandshake() {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto info = device_->handshake();
    if (!info) {
        emit error(tr("Handshake failed"));
        return;
    }

    emit connected(
        QString::fromStdString(info->product_id),
        QString::fromStdString(info->serial),
        QString::fromStdString(info->firmware),
        QString::fromStdString(info->app_version)
    );
}

void DeviceWorker::setBrightness(int value) {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto resp = device_->set_brightness(value);
    if (!resp) {
        emit error(tr("Failed to set brightness"));
        return;
    }
    emit brightnessSet(value);
}

void DeviceWorker::setScreenConfig(const QStringList &media, const QString &ratio,
                                   const QString &screenMode, const QString &playMode,
                                   const QStringList &sysinfoLabels,
                                   const QString &settingsPosition,
                                   const QString &settingsColor,
                                   const QString &settingsAlign,
                                   const QStringList &settingsBadges,
                                   int filterOpacity,
                                   const QString &presetId,
                                   const QStringList &sysinfoLabels2,
                                   const QStringList &settingsBadges2,
                                   bool waterfallMode) {
    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    panorama::ScreenConfig config;
    if (!presetId.isEmpty()) {
        config.preset_id = presetId.toStdString();
    }
    for (const auto &m : media) {
        config.media.push_back(m.toStdString());
    }
    config.ratio = ratio.toStdString();
    config.screen_mode = screenMode.toStdString();
    config.play_mode = playMode.toStdString();

    for (const auto &label : sysinfoLabels) {
        config.sysinfo_display.push_back(label.toStdString());
    }

    config.settings.position = settingsPosition.toStdString();
    config.settings.color = settingsColor.toStdString();
    config.settings.align = settingsAlign.toStdString();
    config.settings.filter_opacity = filterOpacity;
    for (const auto &badge : settingsBadges) {
        config.settings.badges.push_back(badge.toStdString());
    }

    config.waterfall_mode = waterfallMode;

    // Screen Splitting: populate second set of settings and sysinfo
    if (screenMode == "Screen Splitting") {
        for (const auto &label : sysinfoLabels2) {
            config.sysinfo_display2.push_back(label.toStdString());
        }
        config.settings2.position = settingsPosition.toStdString();
        config.settings2.color = settingsColor.toStdString();
        config.settings2.align = settingsAlign.toStdString();
        config.settings2.filter_opacity = filterOpacity;
        for (const auto &badge : settingsBadges2) {
            config.settings2.badges.push_back(badge.toStdString());
        }
    }

    auto resp = device_->set_screen_config(config);
    if (!resp) {
        emit error(tr("Failed to set display configuration"));
        return;
    }

    // Send sysinfoDisplay as separate command if metrics are selected
    if (!config.sysinfo_display.empty()) {
        device_->set_sysinfo_display(config);
    }

    // Send config with hardware names for badges
    std::string cpuName = "Unknown CPU";
    std::string gpuName = "Unknown GPU";

    // Read CPU name from /proc/cpuinfo
    {
        std::ifstream cpuFile("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuFile, line)) {
            if (line.find("model name") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos && pos + 2 < line.size()) {
                    cpuName = line.substr(pos + 2);
                }
                break;
            }
        }
    }

    // Read GPU name: try sysfs product_name first, fallback to lspci
    {
        namespace fs = std::filesystem;
        std::string drmPath = "/sys/class/drm";
        if (fs::exists(drmPath)) {
            for (const auto& entry : fs::directory_iterator(drmPath)) {
                std::string name = entry.path().filename().string();
                if (name.find("card") == 0 && name.find('-') == std::string::npos) {
                    std::string productPath = entry.path().string() + "/device/product_name";
                    std::ifstream gpuFile(productPath);
                    if (gpuFile) {
                        std::string readName;
                        std::getline(gpuFile, readName);
                        if (!readName.empty()) {
                            gpuName = readName;
                            break;
                        }
                    }
                }
            }
        }
        // Fallback 1: glxinfo gives clean name like "AMD Radeon RX 7900 XTX"
        if (gpuName == "Unknown GPU") {
            FILE* pipe = popen("glxinfo 2>/dev/null | grep 'OpenGL renderer' | head -1", "r");
            if (pipe) {
                char buf[512];
                if (fgets(buf, sizeof(buf), pipe)) {
                    std::string line(buf);
                    auto pos = line.find(": ");
                    if (pos != std::string::npos) {
                        gpuName = line.substr(pos + 2);
                        // Cut at first '(' - remove "(radeonsi, navi31, ...)"
                        auto paren = gpuName.find('(');
                        if (paren != std::string::npos)
                            gpuName = gpuName.substr(0, paren);
                        while (!gpuName.empty() && (gpuName.back() == '\n' || gpuName.back() == '\r' || gpuName.back() == ' '))
                            gpuName.pop_back();
                    }
                }
                pclose(pipe);
            }
        }
        // Fallback 2: lspci
        if (gpuName == "Unknown GPU" || gpuName.empty()) {
            FILE* pipe = popen("lspci 2>/dev/null | grep -i 'VGA\\|3D controller' | head -1", "r");
            if (pipe) {
                char buf[512];
                if (fgets(buf, sizeof(buf), pipe)) {
                    std::string line(buf);
                    auto pos = line.find(": ");
                    if (pos != std::string::npos) {
                        gpuName = line.substr(pos + 2);
                        while (!gpuName.empty() && (gpuName.back() == '\n' || gpuName.back() == '\r'))
                            gpuName.pop_back();
                    }
                }
                pclose(pipe);
            }
        }
    }

    fprintf(stderr, "[config] cpu='%s' gpu='%s'\n", cpuName.c_str(), gpuName.c_str());

    // Send full config (KANALI format) - sets everything in one command
    device_->send_full_config(config, cpuName, gpuName, 75, "Celsius");

    emit screenConfigSet();
}

void DeviceWorker::sendSysinfo(const QStringList &labels, const QStringList &values,
                               const QStringList &units) {
    if (!device_ || !device_->is_connected()) {
        return;
    }

    std::vector<panorama::SysinfoData> data;
    for (int i = 0; i < labels.size() && i < values.size() && i < units.size(); ++i) {
        panorama::SysinfoData item;
        item.label = labels[i].toStdString();
        item.value = values[i].toStdString();
        item.unit = units[i].toStdString();
        data.push_back(item);
    }

    device_->send_sysinfo(data);
    emit sysinfoSent();
}

void DeviceWorker::deleteMedia(const QStringList &files) {
    std::vector<std::string> filenames;
    for (const auto &f : files) {
        filenames.push_back(f.toStdString());
    }

    if (!device_ || !device_->is_connected()) {
        emit error(tr("Device not connected"));
        return;
    }

    auto resp = device_->delete_media(filenames);
    if (!resp) {
        emit error(tr("Failed to delete media files"));
        return;
    }

    for (const auto &f : files) {
        panorama::Adb::remove(f.toStdString());
    }

    emit mediaDeleted();
}

void DeviceWorker::uploadMedia(const QString &localPath) {
    if (!panorama::Adb::is_device_connected()) {
        emit error(tr("ADB device not found"));
        return;
    }

    std::string path = localPath.toStdString();
    auto type = panorama::Media::detect_type(path);

    std::string remoteName;
    std::string uploadPath = path;

    if (panorama::Media::needs_conversion(path)) {
        remoteName = panorama::Media::get_converted_name(path);
    } else {
        remoteName = panorama::Media::get_filename(path);
    }

    // Check if file already exists on device - skip upload
    if (panorama::Adb::file_exists(remoteName)) {
        emit mediaUploaded(QString::fromStdString(remoteName));
        return;
    }

    // Need to upload - convert if necessary
    if (panorama::Media::needs_conversion(path)) {
        if (!panorama::Media::is_ffmpeg_available()) {
            emit error(tr("ffmpeg not found. Install it with your system package manager"));
            return;
        }
        emit uploadProgress(tr("Converting to MP4..."));
        std::string converted = std::string(panorama::Media::TMP_DIR) + remoteName;
        bool ok = (type == panorama::MediaType::Gif)
            ? panorama::Media::convert_gif_to_mp4(path, converted)
            : panorama::Media::convert_to_mp4(path, converted);
        if (!ok) {
            emit error(tr("Conversion to MP4 failed"));
            return;
        }
        uploadPath = converted;
    }

    emit uploadProgress(tr("Uploading to device..."));
    if (!panorama::Adb::push(uploadPath, remoteName)) {
        emit error(tr("Upload to device failed"));
        return;
    }

    emit mediaUploaded(QString::fromStdString(remoteName));
}

void DeviceWorker::uploadPreparedPrinterMedia(const QString &devicePath,
                                              const QString &uploadPath,
                                              const QString &remoteName,
                                              const QString &expectedSha256,
                                              const QString &operationId,
                                              quint64 generation) {
    if (!printerGenerationIsCurrent(generation)) {
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::Cancelled,
            tr("Printer-class upload was cancelled because the USB device changed"),
            generation);
        return;
    }
    const QFileInfo preparedInfo(uploadPath);
    if (!preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() || preparedInfo.size() <= 0 ||
        !isSha256Hex(expectedSha256)) {
        emit printerPreparedFileConsumed(uploadPath);
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            tr("Prepared printer-class media is not available"), generation);
        return;
    }

    QString uploadedName;
    QString errorMessage;
    PrinterProtocol::OperationContext context;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    emit printerForegroundProgress(
        operationId, QStringLiteral("Beginning"), 0, preparedInfo.size(),
        tr("Starting printer-class upload..."), generation);
    PrinterProtocol::MutationDetails mutationDetails;
    const bool ok = printerProtocol_->uploadMedia(
        devicePath,
        uploadPath,
        remoteName,
        &uploadedName,
        &errorMessage,
        [this, operationId, generation](qint64 bytesSent, qint64 totalBytes) {
            const int percent = totalBytes > 0
                ? static_cast<int>((bytesSent * 100) / totalBytes)
                : 0;
            emit printerUploadProgress(
                tr("Uploading to printer-class firmware... %1%")
                    .arg(qBound(0, percent, 100)),
                generation);
            emit printerForegroundProgress(
                operationId, QStringLiteral("Transferring"), bytesSent,
                totalBytes,
                tr("Uploading to printer-class firmware... %1%")
                    .arg(qBound(0, percent, 100)),
                generation);
        },
        context, &mutationDetails, expectedSha256);
    if (!ok) {
        if (mutationDetails.outcome ==
            PrinterProtocol::MutationOutcome::FinalizationUnknown) {
            // Every data chunk was acknowledged and FileTransmitEnd was
            // fully written. Do not replay the upload. Recover the session
            // and let DeviceManager reconcile the exact name and size through
            // a read-only FileList request.
            schedulePrinterSessionRecovery(errorMessage, generation);
        } else if (mutationDetails.outcome ==
                   PrinterProtocol::MutationOutcome::PartialOrUnknown) {
            // FileTransmit has no confirmed abort command. A generic USB
            // reset only recreates the host transport and does not prove that
            // the firmware discarded its partial transfer state. Fail closed
            // for this physical device epoch and require an observed power
            // cycle before any further mutation.
            markPrinterSessionLost(
                errorMessage.isEmpty()
                    ? tr("The PASE upload outcome is partial or unknown; physically reconnect the device before continuing")
                    : tr("The PASE upload outcome is partial or unknown: %1. Physically reconnect the device before continuing")
                          .arg(errorMessage),
                generation);
        } else {
            schedulePrinterSessionRecovery(errorMessage, generation);
        }
        emit printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            mutationDetails.outcome,
            tr("Printer-class upload failed: %1").arg(errorMessage),
            generation);
        return;
    }

    restartPrinterKeepaliveAfterActivity();
    emit printerUploadFinished(
        operationId, uploadPath, uploadedName, true,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation);
}

void DeviceWorker::refreshMediaList() {
    if (!panorama::Adb::is_device_connected()) {
        emit error(tr("ADB device not found"));
        return;
    }

    auto files = panorama::Adb::list_media();
    if (!files) {
        emit error(tr("Failed to retrieve file list"));
        return;
    }

    QStringList list;
    for (const auto &f : *files) {
        if (!f.empty()) {
            list.append(QString::fromStdString(f));
        }
    }
    emit mediaListReady(list);
}


void DeviceWorker::configurePrinterDevice(const QString &devicePath,
                                          const QString &deviceSerial,
                                          quint64 generation) {
    stopPrinterSession();
    printerProtocol_ = std::make_unique<PrinterProtocol>();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerDevicePath_ = devicePath;
    printerDeviceSerial_ = deviceSerial.trimmed();
    foregroundPrinterOperationId_.clear();
    printerOverlayConfig_ = {};
    configuredPrinterGeneration_ = generation;
    printerSessionElapsedTimer_.start();
    drainAllPrinterCancellations();
    logPrinterLifecycleEvent(
        QStringLiteral("endpoint_configured"), generation,
        {
            {QStringLiteral("device_path"), devicePath},
            {QStringLiteral("serial"), printerDeviceSerial_},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(printerOverlayLeaseMode_)}
        });
}

void DeviceWorker::restorePrinterOverlay(
                                         const PrinterProtocol::PaseOverlayConfig &overlay,
                                         quint64 generation) {
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        return;
    }
    printerOverlayConfig_ = overlay;
    if (printerSessionState_ == PrinterSessionState::Active &&
        paseOverlayHasContent(printerOverlayConfig_)) {
        transitionPrinterSessionState(
            PrinterSessionState::AwaitingOverlayActivation,
            QStringLiteral("overlay_activation_pending"));
        printerOverlayActivationPending_ = true;
        printerOverlayLeaseRefreshNext_ = false;
        printerMetricsTimer_->stop();
        emit printerSessionStopped(generation);
        restartPrinterKeepaliveAfterActivity();
        return;
    }
    if (printerSessionState_ == PrinterSessionState::Active) {
        startPrinterMetrics();
    }
}

void DeviceWorker::beginPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    if (operationId.isEmpty() || generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ != PrinterSessionState::Active) {
        return;
    }
    foregroundPrinterOperationId_ = operationId;
    printerKeepaliveTimer_->stop();
    printerMetricsTimer_->stop();
}

void DeviceWorker::endPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    if (operationId.isEmpty() ||
        foregroundPrinterOperationId_ != operationId) {
        return;
    }
    foregroundPrinterOperationId_.clear();
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ != PrinterSessionState::Active) {
        return;
    }
    startPrinterMetrics();
    restartPrinterKeepaliveAfterActivity();
}

void DeviceWorker::clearPrinterDevice(quint64 generation) {
    if (generation < configuredPrinterGeneration_) {
        return;
    }
    stopPrinterSession();
    printerProtocol_ = std::make_unique<PrinterProtocol>();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerDevicePath_.clear();
    printerDeviceSerial_.clear();
    foregroundPrinterOperationId_.clear();
    printerOverlayConfig_ = {};
    configuredPrinterGeneration_ = generation;
    printerSessionElapsedTimer_.invalidate();
    drainAllPrinterCancellations();
}

void DeviceWorker::readPrinterDeviceInfo(const QString &devicePath,
                                         quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::Result result =
        printerProtocol_->readDeviceInfo(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerDeviceInfoFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerDeviceInfoReady(result.deviceInfo, generation);
}

void DeviceWorker::readPrinterDisplayState(const QString &devicePath,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::PaseDisplayStateResult result =
        printerProtocol_->readPaseDisplayState(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerDisplayStateFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerDisplayStateReady(result.state, generation);
}

void DeviceWorker::refreshPrinterMediaList(const QString &devicePath,
                                           const QString &operationId,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::MediaListResult result =
        printerProtocol_->readMediaList(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit printerMediaListFailed(
            operationId,
            tr("Failed to read printer-class media list: %1").arg(result.error),
            generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerMediaListReady(operationId, result.files, generation);
}

void DeviceWorker::deletePrinterMedia(
    const QString &devicePath, const QStringList &fileNames,
    const QString &operationId, const QString &deleteIntentPath,
    bool reconcileOnly, quint64 generation) {
    PrinterProtocol::OperationContext initialContext;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &initialContext, &errorMessage)) {
        emit printerDeleteFinished(
            operationId, fileNames, {}, {}, false,
            reconcileOnly
                ? PrinterProtocol::MutationOutcome::PartialOrUnknown
                : printerOperationIsCancelled(operationId)
                    ? PrinterProtocol::MutationOutcome::Cancelled
                    : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, initialContext,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerDeleteFinished(
            operationId, fileNames, {}, {}, false,
            reconcileOnly
                ? PrinterProtocol::MutationOutcome::PartialOrUnknown
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }

    PrinterProtocol::OperationContext stableContext;
    stableContext.cancellationFd = printerCancellationFd_;
    stableContext.isCancelled = [this, generation]() {
        return !printerGenerationIsCurrent(generation);
    };
    stableContext.maintainKeepalive = true;

    QStringList confirmedDeleted;
    const auto persistIntent =
        [this, &deleteIntentPath, &operationId, &fileNames,
         &confirmedDeleted, generation](
            int index, const PrinterProtocol::MediaFile &media,
            QString *persistenceError) {
            if (printerOperationIsCancelled(operationId)) {
                if (persistenceError) {
                    *persistenceError = tr(
                        "Deletion was cancelled before FileRemove dispatch");
                }
                return false;
            }
            QJsonObject intent;
            intent.insert(QStringLiteral("version"),
                          kDeleteIntentFormatVersion);
            intent.insert(QStringLiteral("updatedUtc"),
                          QDateTime::currentDateTimeUtc().toString(
                              Qt::ISODateWithMs));
            intent.insert(QStringLiteral("operationId"), operationId);
            intent.insert(QStringLiteral("deviceIdentity"),
                          printerDeviceSerial_.trimmed());
            intent.insert(QStringLiteral("deviceGeneration"),
                          QString::number(generation));
            intent.insert(QStringLiteral("requestedNames"),
                          stringListToJson(fileNames));
            intent.insert(QStringLiteral("deletedNames"),
                          stringListToJson(confirmedDeleted));
            intent.insert(QStringLiteral("currentIndex"), index);
            intent.insert(QStringLiteral("currentName"), media.name);
            intent.insert(QStringLiteral("currentSize"),
                          QString::number(media.size));
            intent.insert(QStringLiteral("currentSource"), 1);
            intent.insert(QStringLiteral("currentReadOnly"),
                          media.readOnly);
            intent.insert(QStringLiteral("stage"),
                          QStringLiteral("Dispatch"));
            intent.insert(QStringLiteral("mayHaveStarted"), true);
            return writeJsonObjectAtomically(deleteIntentPath, intent,
                                             persistenceError);
        };
    const auto progress =
        [this, &deleteIntentPath, &operationId, &fileNames,
         &confirmedDeleted, generation](
            const QString &stage, const QString &fileName,
            int completedFiles, int totalFiles) {
            if (completedFiles > confirmedDeleted.size()) {
                confirmedDeleted = fileNames.mid(0, completedFiles);
            }
            emit printerForegroundProgress(
                operationId, stage, completedFiles, totalFiles,
                stage == QStringLiteral("DeletePreflight")
                    ? tr("Checking whether %1 can be deleted...")
                          .arg(fileName)
                    : stage == QStringLiteral("Deleting")
                        ? tr("Sending one delete request for %1...")
                              .arg(fileName)
                        : tr("Verifying deletion of %1 through FileList...")
                              .arg(fileName),
                generation);
            if (deleteIntentPath.isEmpty() ||
                !QFileInfo::exists(deleteIntentPath)) {
                return;
            }
            QFile intentFile(deleteIntentPath);
            if (!intentFile.open(QIODevice::ReadOnly)) {
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                intentFile.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError ||
                !document.isObject()) {
                return;
            }
            QJsonObject intent = document.object();
            intent.insert(QStringLiteral("updatedUtc"),
                          QDateTime::currentDateTimeUtc().toString(
                              Qt::ISODateWithMs));
            intent.insert(QStringLiteral("stage"), stage);
            intent.insert(QStringLiteral("deletedNames"),
                          stringListToJson(confirmedDeleted));
            QString ignored;
            writeJsonObjectAtomically(deleteIntentPath, intent, &ignored);
        };

    const PrinterProtocol::DeleteResult result =
        printerProtocol_->removeUserMedia(
            devicePath, fileNames,
            reconcileOnly ? PrinterProtocol::BeforeDeleteDispatch{}
                          : persistIntent,
            progress, stableContext, reconcileOnly);
    if (!result.success &&
        result.outcome ==
            PrinterProtocol::MutationOutcome::PartialOrUnknown) {
        schedulePrinterSessionRecovery(result.error, generation);
    } else {
        restartPrinterKeepaliveAfterActivity();
    }
    emit printerDeleteFinished(
        operationId, fileNames, result.deletedNames, result.files,
        result.success, result.outcome, result.error, generation);
}

void DeviceWorker::applyPrinterMedia(const QString &devicePath,
                                     const QString &mediaFile,
                                     const TryxRuntimeApplyRequest &request,
                                     bool updateMetrics,
                                     const QString &operationId,
                                     quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerApplyFinished(
            operationId, mediaFile, false, updateMetrics,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        emit printerApplyFinished(
            operationId, mediaFile, false, updateMetrics,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        schedulePrinterSessionRecovery(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const QString operationSubject = mediaFile.isEmpty()
        ? tr("display settings")
        : mediaFile;
    emit printerUploadProgress(
        tr("Applying printer-class configuration: %1")
            .arg(operationSubject),
        generation);
    emit printerForegroundProgress(
        operationId, QStringLiteral("Applying"), 0, 0,
        tr("Applying printer-class configuration: %1")
            .arg(operationSubject),
        generation);
    PrinterProtocol::MutationDetails mutationDetails;
    const bool replaceRequested =
        updateMetrics || request.replaceOverlay;
    const bool rebuildOverlay =
        replaceRequested || request.display.orientationPresent;
    PrinterProtocol::PaseOverlayConfig overlay = replaceRequested
        ? paseOverlayFromApplyRequest(request)
        : printerOverlayConfig_;
    if (request.display.orientationPresent) {
        overlay.waterfallMode = request.display.waterfallMode;
    }
    hydratePaseBadgeText(&overlay, printerSystemMonitor_);

    PrinterProtocol::PaseApplyConfig applyConfig;
    applyConfig.media = request.media;
    if (applyConfig.media.isEmpty() && !mediaFile.isEmpty()) {
        applyConfig.media = {mediaFile};
    }
    applyConfig.screenMode = request.screenMode;
    applyConfig.playMode = request.playMode;
    applyConfig.mediaPresent = !applyConfig.media.isEmpty();
    applyConfig.replaceOverlay = rebuildOverlay;
    applyConfig.display.brightnessPresent =
        request.display.brightnessPresent;
    applyConfig.display.brightness =
        request.display.brightness;
    applyConfig.display.standbyPresent =
        request.display.standbyPresent;
    applyConfig.display.standbyEnabled =
        request.display.standbyEnabled;
    applyConfig.display.backlightPresent =
        request.display.backlightPresent;
    applyConfig.display.backlightEnabled =
        request.display.backlightEnabled;
    applyConfig.display.orientationPresent =
        request.display.orientationPresent;
    applyConfig.display.mirrorMode =
        request.display.mirrorMode;
    applyConfig.display.waterfallMode =
        request.display.waterfallMode;
    applyConfig.overlay = overlay;

    PrinterProtocol::PaseDisplayState appliedState;
    if (!printerProtocol_->applyPaseConfiguration(
            devicePath, applyConfig, &errorMessage, context,
            &mutationDetails, &appliedState)) {
        qWarning().noquote()
            << QStringLiteral(
                   "PASE apply failed: operation=%1 generation=%2 stage=%3 outcome=%4 screen_mode=%5 media_count=%6 error=%7")
                   .arg(operationId)
                   .arg(generation)
                   .arg(mutationDetails.stage)
                   .arg(static_cast<int>(mutationDetails.outcome))
                   .arg(applyConfig.screenMode)
                   .arg(applyConfig.media.size())
                   .arg(errorMessage);
        if (mutationDetails.outcome ==
            PrinterProtocol::MutationOutcome::VerificationFailed) {
            emit printerDisplayStateReady(
                appliedState, generation);
        }
        const bool requiresSessionRecovery =
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::VerificationFailed &&
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::Rejected;
        emit printerApplyFinished(
            operationId, mediaFile, false, rebuildOverlay,
            mutationDetails.outcome,
            tr("Failed to apply printer-class configuration: %1")
                .arg(errorMessage),
            generation);
        if (requiresSessionRecovery) {
            schedulePrinterSessionRecovery(
                errorMessage, generation);
        }
        return;
    }

    if (rebuildOverlay) {
        printerOverlayConfig_ = overlay;
        printerOverlayLeaseRefreshNext_ = false;
    }
    startPrinterMetrics();
    emit printerDisplayStateReady(appliedState, generation);
    emit printerUploadProgress(
        tr("Printer-class configuration applied"), generation);
    emit printerApplyFinished(
        operationId, mediaFile, true, rebuildOverlay,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation);
    restartPrinterKeepaliveAfterActivity();
}

void DeviceWorker::configurePrinterMetrics(
    const QString &devicePath,
    const TryxRuntimeMetricsConfigRequest &request,
    const QString &operationId, quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit printerMetricsConfigured(
            operationId, false,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerMetricsConfigured(
            operationId, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    emit printerForegroundProgress(
        operationId, QStringLiteral("ConfiguringMetrics"), 0, 0,
        tr("Configuring PASE metrics layout..."), generation);
    PrinterProtocol::PaseOverlayConfig overlay =
        paseOverlayFromMetricsRequest(request);
    hydratePaseBadgeText(&overlay, printerSystemMonitor_);
    PrinterProtocol::MutationDetails mutationDetails;
    if (!printerProtocol_->configurePaseOverlay(
            devicePath, overlay, &errorMessage, context,
            &mutationDetails)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerMetricsConfigured(
            operationId, false, mutationDetails.outcome,
            tr("Failed to configure PASE metrics: %1").arg(errorMessage),
            generation);
        return;
    }
    printerOverlayConfig_ = overlay;
    printerOverlayLeaseRefreshNext_ = false;
    startPrinterMetrics();
    emit printerMetricsConfigured(
        operationId, true, PrinterProtocol::MutationOutcome::Succeeded,
        QString(), generation);
    restartPrinterKeepaliveAfterActivity();
}

void DeviceWorker::sendPrinterSysinfo(
    const QString &devicePath, const QStringList &labels,
    const QStringList &values, const QStringList &units,
    quint64 generation) {
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        emit printerSysinfoSent(generation);
        return;
    }
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage) ||
        !ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    if (!printerProtocol_->sendPaseMetricBatch(
            devicePath, printerOverlayConfig_, labels, values, units,
            &errorMessage, context)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }
    updatePrinterOverlayInitialMetrics(labels, values, units);
    restartPrinterKeepaliveAfterActivity();
    emit printerSysinfoSent(generation);
}

void DeviceWorker::startPrinterMetrics() {
    printerMetricsTimer_->stop();
    if (!foregroundPrinterOperationId_.isEmpty() ||
        printerSessionState_ != PrinterSessionState::Active ||
        !printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        return;
    }

    QStringList labels;
    QStringList values;
    QStringList units;
    collectCurrentPrinterMetrics(&labels, &values, &units);
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        return;
    }
    printerMetricsTimer_->start();
}

void DeviceWorker::sendPrinterMetrics() {
    if (printerSessionState_ != PrinterSessionState::Active ||
        !foregroundPrinterOperationId_.isEmpty() ||
        !printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        return;
    }

    QStringList labels;
    QStringList values;
    QStringList units;
    collectCurrentPrinterMetrics(&labels, &values, &units);
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        return;
    }

    PrinterProtocol::OperationContext context;
    QString errorMessage;
    const quint64 generation = configuredPrinterGeneration_;
    if (!preparePrinterOperation(printerDevicePath_, generation, QString(),
                                 &context, &errorMessage)) {
        return;
    }
    context.maintainKeepalive = true;
    if (!printerProtocol_->sendPaseMetricBatch(
            printerDevicePath_, printerOverlayConfig_, labels, values, units,
            &errorMessage, context)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit printerSysinfoFailed(errorMessage, generation);
        return;
    }

    updatePrinterOverlayInitialMetrics(labels, values, units);
    // Metric updates are headerless KANALI UI commands, not the UDB watchdog
    // Ping. Keep the independent two-second Ping schedule even while one-second
    // metric samples are active.
    emit printerSysinfoSent(generation);
}

void DeviceWorker::collectCurrentPrinterMetrics(QStringList *labels,
                                                QStringList *values,
                                                QStringList *units) {
    collectPaseMetricValues(printerSystemMonitor_, labels, values, units);
    QStringList availableMetrics = *labels;
    availableMetrics.append(QStringLiteral("Date&Time"));
    emit printerMetricsAvailabilityChanged(availableMetrics,
                                           configuredPrinterGeneration_);
}

void DeviceWorker::updatePrinterOverlayInitialMetrics(
    const QStringList &labels, const QStringList &values,
    const QStringList &units) {
    printerOverlayConfig_.left.initialLabels = labels;
    printerOverlayConfig_.left.initialValues = values;
    printerOverlayConfig_.left.initialUnits = units;
    printerOverlayConfig_.right.initialLabels = labels;
    printerOverlayConfig_.right.initialValues = values;
    printerOverlayConfig_.right.initialUnits = units;
}

void DeviceWorker::startPrinterDisplaySession(const QString &devicePath,
                                              quint64 generation) {
    if (printerSessionState_ == PrinterSessionState::Active ||
        printerSessionState_ ==
            PrinterSessionState::AwaitingProtocolReadiness ||
        printerSessionState_ == PrinterSessionState::Starting ||
        printerSessionState_ ==
            PrinterSessionState::AwaitingOverlayActivation ||
        printerSessionState_ == PrinterSessionState::Recovering ||
        (printerSessionState_ == PrinterSessionState::Lost &&
         generation == configuredPrinterGeneration_)) {
        return;
    }
    printerSessionRecoveryAttempt_ = 0;
    attemptPrinterSessionStart(devicePath, generation);
}

void DeviceWorker::retryPrinterSessionStart() {
    if (printerSessionState_ != PrinterSessionState::Recovering) {
        return;
    }
    markPrinterSessionLost(
        tr("Automatic same-generation PASE bootstrap retry is disabled; physically reconnect the device before continuing"),
        configuredPrinterGeneration_);
}

void DeviceWorker::attemptPrinterSessionStart(const QString &devicePath,
                                              quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(), &context,
                                 &errorMessage)) {
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage, true)) {
        if (!printerGenerationIsCurrent(generation)) {
            return;
        }
        if (printerSessionState_ == PrinterSessionState::Lost ||
            printerSessionState_ ==
                PrinterSessionState::AwaitingOverlayActivation) {
            return;
        }
        markPrinterSessionLost(
            errorMessage.isEmpty()
                ? tr("PASE display session bootstrap failed; physically reconnect the device before continuing")
                : tr("PASE display session bootstrap failed: %1. Physically reconnect the device before continuing")
                      .arg(errorMessage),
            generation);
        return;
    }
    printerSessionRecoveryAttempt_ = 0;
}

void DeviceWorker::sendPrinterKeepalive() {
    if ((printerSessionState_ != PrinterSessionState::Active &&
         printerSessionState_ !=
             PrinterSessionState::AwaitingOverlayActivation) ||
        !foregroundPrinterOperationId_.isEmpty()) {
        return;
    }
    const quint64 generation = configuredPrinterGeneration_;
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(printerDevicePath_, generation, QString(),
                                 &context, &errorMessage)) {
        if (printerGenerationIsCurrent(generation)) {
            markPrinterSessionLost(
                tr("PASE keepalive was cancelled before a safe write could start"),
                generation);
        } else {
            stopPrinterSession();
        }
        return;
    }

    const bool refreshOverlayLease =
        printerSessionState_ == PrinterSessionState::Active &&
        printerOverlayLeaseMode_ ==
            PrinterOverlayLeaseMode::PingAndOverlayLease &&
        printerOverlayLeaseRefreshNext_ &&
        paseOverlayHasContent(printerOverlayConfig_);
    const QString commandClass =
        printerSessionState_ ==
                PrinterSessionState::AwaitingOverlayActivation
            ? QStringLiteral("post-bootstrap-ping")
            : (refreshOverlayLease
                   ? QStringLiteral("overlay-lease")
                   : QStringLiteral("ping"));
    logPrinterLifecycleEvent(
        QStringLiteral("command_started"), generation,
        {
            {QStringLiteral("command_class"), commandClass},
            {QStringLiteral("session_state"),
             printerSessionStateName(printerSessionState_)},
            {QStringLiteral("retry_attempt"),
             QString::number(printerKeepaliveRetryCount_)}
        });
    const PrinterProtocol::KeepaliveOutcome outcome =
        refreshOverlayLease
            ? printerProtocol_->sendDisplayKeepalive(
                  printerDevicePath_, &errorMessage, context,
                  &printerOverlayConfig_)
            : printerProtocol_->sendKeepalive(
                  printerDevicePath_, &errorMessage, context);
    logPrinterLifecycleEvent(
        QStringLiteral("command_completed"), generation,
        {
            {QStringLiteral("command_class"), commandClass},
            {QStringLiteral("outcome"),
             printerKeepaliveOutcomeName(outcome)},
            {QStringLiteral("retry_attempt"),
             QString::number(printerKeepaliveRetryCount_)}
        });
    if (!printerGenerationIsCurrent(generation)) {
        stopPrinterSession();
        return;
    }
    if (outcome == PrinterProtocol::KeepaliveOutcome::RetryableFailure &&
        printerSessionState_ == PrinterSessionState::Active &&
        printerGenerationIsCurrent(generation) &&
        printerKeepaliveRetryCount_ < kMaxPrinterKeepaliveWriteRetries) {
        ++printerKeepaliveRetryCount_;
        emit printerUploadProgress(
            tr("Printer-class keepalive write will be retried (%1/%2): %3")
                .arg(printerKeepaliveRetryCount_)
                .arg(kMaxPrinterKeepaliveWriteRetries)
                .arg(errorMessage),
            generation);
        printerKeepaliveTimer_->start(
            qMin(2000, kPrinterKeepaliveRetryBackoffMs *
                           printerKeepaliveRetryCount_));
        return;
    }
    if (outcome != PrinterProtocol::KeepaliveOutcome::Sent) {
        if (printerSessionState_ ==
            PrinterSessionState::AwaitingOverlayActivation) {
            markPrinterSessionLost(
                errorMessage.isEmpty()
                    ? tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed")
                    : tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed: %1")
                          .arg(errorMessage),
                generation);
            return;
        }
        const QString stoppedMessage =
            outcome == PrinterProtocol::KeepaliveOutcome::RetryableFailure
            ? (refreshOverlayLease
                   ? tr("PASE overlay lease refresh stopped after %1 retries: %2")
                   : tr("Printer-class keepalive stopped after %1 retries: %2"))
                  .arg(kMaxPrinterKeepaliveWriteRetries)
                  .arg(errorMessage)
            : (refreshOverlayLease
                   ? tr("PASE overlay lease refresh stopped: %1")
                   : tr("Printer-class keepalive stopped: %1"))
                  .arg(errorMessage);
        if (refreshOverlayLease) {
            markPrinterSessionLost(
                tr("PASE overlay lease could not be refreshed safely: %1")
                    .arg(stoppedMessage),
                generation);
            return;
        }
        schedulePrinterSessionRecovery(stoppedMessage, generation);
        return;
    }
    const bool recovered = printerKeepaliveRetryCount_ > 0;
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        activateRestoredPrinterOverlay(generation);
        return;
    }
    if (printerOverlayLeaseMode_ ==
            PrinterOverlayLeaseMode::PingAndOverlayLease &&
        paseOverlayHasContent(printerOverlayConfig_)) {
        printerOverlayLeaseRefreshNext_ =
            !printerOverlayLeaseRefreshNext_;
    } else {
        printerOverlayLeaseRefreshNext_ = false;
    }
    restartPrinterKeepaliveAfterActivity();
    emit printerTransportReady(generation);
    if (recovered) {
        emit printerUploadProgress(
            tr("PASE display keepalive recovered"), generation);
    }
}

bool DeviceWorker::preparePrinterOperation(
    const QString &devicePath, quint64 generation,
    const QString &operationId,
    PrinterProtocol::OperationContext *context, QString *errorMessage) {
    if (devicePath.isEmpty() || devicePath != printerDevicePath_ ||
        generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        if (errorMessage) {
            *errorMessage =
                tr("Printer-class operation was cancelled because the USB device changed");
        }
        return false;
    }

    if (printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = tr("Printer-class operation was cancelled by the user");
        }
        return false;
    }
    const int cancellationFd = operationId.isEmpty()
        ? printerCancellationFd_
        : printerOperationCancellationFd_;
    drainPrinterCancellation(cancellationFd);
    if (!printerGenerationIsCurrent(generation) ||
        printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = printerOperationIsCancelled(operationId)
                ? tr("Printer-class operation was cancelled by the user")
                : tr("Printer-class operation was cancelled because the USB device changed");
        }
        return false;
    }
    if (context) {
        context->cancellationFd = cancellationFd;
        context->isCancelled = [this, generation, operationId]() {
            return !printerGenerationIsCurrent(generation) ||
                   printerOperationIsCancelled(operationId);
        };
    }
    return true;
}

bool DeviceWorker::ensurePrinterSession(
    const QString &devicePath, quint64 generation,
    const PrinterProtocol::OperationContext &context, QString *errorMessage,
    bool allowPendingOverlayActivation) {
    if (printerSessionState_ == PrinterSessionState::Active) {
        return true;
    }
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        if (allowPendingOverlayActivation) {
            return true;
        }
        if (errorMessage) {
            *errorMessage = tr(
                "The PASE protocol session is waiting for confirmed overlay restoration");
        }
        return false;
    }
    if (printerSessionState_ ==
            PrinterSessionState::AwaitingProtocolReadiness ||
        printerSessionState_ == PrinterSessionState::Starting) {
        if (errorMessage) {
            *errorMessage = tr("Printer-class display session is already starting");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Lost) {
        if (errorMessage) {
            *errorMessage = tr(
                "Printer-class display session is lost until a new USB endpoint generation appears");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Recovering) {
        if (errorMessage) {
            *errorMessage = tr(
                "Automatic same-generation PASE bootstrap retry is disabled");
        }
        return false;
    }

    transitionPrinterSessionState(
        PrinterSessionState::AwaitingProtocolReadiness,
        QStringLiteral("protocol_readiness_started"));
    printerKeepaliveTimer_->stop();
    printerMetricsTimer_->stop();
    emit printerUploadProgress(tr("Starting PASE display session..."), generation);
    PrinterProtocol::OperationContext sessionContext = context;
    sessionContext.onReadinessProbeRetry =
        [this, generation](
            const PrinterProtocol::ReadinessRetryInfo &retry) {
            if (generation != configuredPrinterGeneration_ ||
                !printerGenerationIsCurrent(generation) ||
                printerSessionState_ !=
                    PrinterSessionState::AwaitingProtocolReadiness) {
                return;
            }
            logPrinterLifecycleEvent(
                QStringLiteral(
                    "readiness_probe_safely_retried"),
                generation,
                {
                    {QStringLiteral("command_class"),
                     QStringLiteral("device-info")},
                    {QStringLiteral("retry_attempt"),
                     QString::number(retry.attempt)},
                    {QStringLiteral("expected_bytes"),
                     QString::number(retry.expectedBytes)},
                    {QStringLiteral("actual_bytes"),
                     QString::number(retry.actualBytes)},
                    {QStringLiteral("transfer_status"),
                     retry.transferStatus},
                    {QStringLiteral("backoff_ms"),
                     QString::number(retry.backoffMs)},
                    {QStringLiteral("elapsed_ms"),
                     QString::number(retry.elapsedMs)}
                });
        };
    sessionContext.onDeviceInfoReady = [this, generation]() {
        if (generation != configuredPrinterGeneration_ ||
            !printerGenerationIsCurrent(generation) ||
            printerSessionState_ !=
                PrinterSessionState::AwaitingProtocolReadiness) {
            return;
        }
        transitionPrinterSessionState(
            PrinterSessionState::Starting,
            QStringLiteral("device_info_confirmed"));
    };
    const PrinterProtocol::Result sessionResult =
        printerProtocol_->startDisplaySession(devicePath,
                                              sessionContext);
    if (!sessionResult.success) {
        const bool persistentFailure =
            printerProtocol_->persistentUsbInputFailure();
        logPrinterLifecycleEvent(
            QStringLiteral("readiness_failed"), generation,
            {
                {QStringLiteral("failure_class"),
                 persistentFailure
                     ? QStringLiteral("persistent-input-transport")
                     : QStringLiteral("bootstrap-failed")},
                {QStringLiteral("elapsed_ms"),
                 printerSessionElapsedTimer_.isValid()
                     ? QString::number(
                           printerSessionElapsedTimer_.elapsed())
                     : QStringLiteral("-1")}
            });
        if (persistentFailure) {
            const QString persistentMessage = tr(
                "The PASE USB interface stopped responding after persistent input transport errors. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.");
            markPrinterSessionLost(persistentMessage, generation);
            if (errorMessage) {
                *errorMessage = persistentMessage;
            }
            return false;
        }
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage = sessionResult.error.isEmpty()
                ? tr("Failed to start the printer-class display session")
                : tr("Failed to start the printer-class display session: %1")
                      .arg(sessionResult.error);
        }
        return false;
    }

    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage =
                tr("Printer-class display session was cancelled because the USB device changed");
        }
        return false;
    }
    if (printerSessionState_ != PrinterSessionState::Starting) {
        logPrinterLifecycleEvent(
            QStringLiteral("readiness_failed"), generation,
            {
                {QStringLiteral("failure_class"),
                 QStringLiteral(
                     "device-info-not-confirmed")},
                {QStringLiteral("elapsed_ms"),
                 printerSessionElapsedTimer_.isValid()
                     ? QString::number(
                           printerSessionElapsedTimer_.elapsed())
                     : QStringLiteral("-1")}
            });
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage = tr(
                "PASE bootstrap completed without an exact DeviceInfo readiness confirmation");
        }
        return false;
    }
    logPrinterLifecycleEvent(
        QStringLiteral("bootstrap_completed"), generation,
        {
            {QStringLiteral("session_state"),
             printerSessionStateName(printerSessionState_)},
            {QStringLiteral("elapsed_ms"),
             printerSessionElapsedTimer_.isValid()
                 ? QString::number(printerSessionElapsedTimer_.elapsed())
                 : QStringLiteral("-1")}
        });

    printerSessionRecoveryAttempt_ = 0;
    transitionPrinterSessionState(
        PrinterSessionState::AwaitingOverlayActivation,
        QStringLiteral("post_bootstrap_ping_pending"));
    printerOverlayActivationPending_ = true;
    printerOverlayLeaseRefreshNext_ = false;
    printerKeepaliveRetryCount_ = 0;
    emit printerUploadProgress(
        tr("PASE protocol session is ready; waiting for a confirmed keepalive before restoring the overlay"),
        generation);
    if (allowPendingOverlayActivation) {
        restartPrinterKeepaliveAfterActivity();
        return true;
    }

    sendPrinterKeepalive();
    if (printerSessionState_ == PrinterSessionState::Active) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = printerSessionState_ == PrinterSessionState::Lost
            ? tr("The mandatory post-bootstrap PASE keepalive failed")
            : tr("The mandatory post-bootstrap PASE keepalive did not activate the display session");
    }
    return false;
}

bool DeviceWorker::printerGenerationIsCurrent(quint64 generation) const {
    return printerEndpointReady_.load(std::memory_order_acquire) &&
           printerGenerationGate_.load(std::memory_order_acquire) == generation;
}

bool DeviceWorker::printerOperationIsCancelled(
    const QString &operationId) const {
    if (operationId.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&printerOperationCancellationMutex_);
    return cancelledPrinterOperationIds_.contains(operationId);
}

void DeviceWorker::drainPrinterCancellation(int cancellationFd) {
    if (cancellationFd < 0) {
        return;
    }
    uint64_t value = 0;
    while (::read(cancellationFd, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
}

void DeviceWorker::drainAllPrinterCancellations() {
    drainPrinterCancellation(printerCancellationFd_);
    drainPrinterCancellation(printerOperationCancellationFd_);
}

QString DeviceWorker::printerSessionStateName(
    PrinterSessionState state) {
    switch (state) {
    case PrinterSessionState::Passive:
        return QStringLiteral("passive");
    case PrinterSessionState::AwaitingProtocolReadiness:
        return QStringLiteral("awaiting-protocol-readiness");
    case PrinterSessionState::Starting:
        return QStringLiteral("starting");
    case PrinterSessionState::AwaitingOverlayActivation:
        return QStringLiteral("awaiting-overlay-activation");
    case PrinterSessionState::Active:
        return QStringLiteral("active");
    case PrinterSessionState::Recovering:
        return QStringLiteral("recovering");
    case PrinterSessionState::Lost:
        return QStringLiteral("lost");
    }
    return QStringLiteral("unknown");
}

void DeviceWorker::transitionPrinterSessionState(
    PrinterSessionState state, const QString &eventName) {
    const PrinterSessionState previous = printerSessionState_;
    printerSessionState_ = state;
    logPrinterLifecycleEvent(
        eventName, configuredPrinterGeneration_,
        {
            {QStringLiteral("state_from"),
             printerSessionStateName(previous)},
            {QStringLiteral("state_to"),
             printerSessionStateName(state)},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(printerOverlayLeaseMode_)},
            {QStringLiteral("device_path"), printerDevicePath_},
            {QStringLiteral("serial"), printerDeviceSerial_},
            {QStringLiteral("recovery_attempt"),
             QString::number(printerSessionRecoveryAttempt_)},
            {QStringLiteral("elapsed_ms"),
             printerSessionElapsedTimer_.isValid()
                 ? QString::number(printerSessionElapsedTimer_.elapsed())
                 : QStringLiteral("-1")}
        });
}

void DeviceWorker::stopPrinterSession() {
    const bool notifyStopped =
        printerSessionState_ != PrinterSessionState::Passive;
    const quint64 stoppedGeneration = configuredPrinterGeneration_;
    if (notifyStopped) {
        transitionPrinterSessionState(
            PrinterSessionState::Passive,
            QStringLiteral("display_session_stopped"));
    }
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerKeepaliveRetryCount_ = 0;
    if (printerKeepaliveTimer_) {
        printerKeepaliveTimer_->stop();
    }
    if (printerMetricsTimer_) {
        printerMetricsTimer_->stop();
    }
    if (printerRecoveryTimer_) {
        printerRecoveryTimer_->stop();
    }
    if (printerProtocol_) {
        printerProtocol_->close();
    }
    if (notifyStopped) {
        emit printerSessionStopped(stoppedGeneration);
    }
}

void DeviceWorker::schedulePrinterSessionRecovery(
    const QString &reason, quint64 generation) {
    if (printerSessionState_ == PrinterSessionState::Lost &&
        generation == configuredPrinterGeneration_) {
        return;
    }
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        return;
    }
    if (printerProtocol_ &&
        printerProtocol_->persistentUsbInputFailure()) {
        const QString persistentMessage = reason.isEmpty()
            ? tr("The PASE USB interface stopped responding after persistent input transport errors. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
            : tr("The PASE USB interface stopped responding after persistent input transport errors: %1. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
                  .arg(reason);
        markPrinterSessionLost(persistentMessage, generation);
        return;
    }
    if (!printerGenerationIsCurrent(generation) ||
        generation != configuredPrinterGeneration_ ||
        printerDevicePath_.isEmpty()) {
        return;
    }
    markPrinterSessionLost(
        reason.isEmpty()
            ? tr("The PASE display session stopped after an operation failure; physically reconnect the device before continuing")
            : tr("The PASE display session stopped after an operation failure: %1. Physically reconnect the device before continuing")
                  .arg(reason),
        generation);
}

void DeviceWorker::restartPrinterKeepaliveAfterActivity() {
    if ((printerSessionState_ == PrinterSessionState::Active ||
         printerSessionState_ ==
             PrinterSessionState::AwaitingOverlayActivation) &&
        foregroundPrinterOperationId_.isEmpty() &&
        printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        printerKeepaliveRetryCount_ = 0;
        printerKeepaliveTimer_->start(
            printerProtocol_->millisecondsUntilKeepalive());
    }
}

void DeviceWorker::activateRestoredPrinterOverlay(
    quint64 generation) {
    if (!printerOverlayActivationPending_ ||
        printerSessionState_ !=
            PrinterSessionState::AwaitingOverlayActivation ||
        generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        return;
    }

    printerOverlayActivationPending_ = false;
    const bool restoreOverlay =
        paseOverlayHasContent(printerOverlayConfig_);
    logPrinterLifecycleEvent(
        QStringLiteral("overlay_activation_started"), generation,
        {
            {QStringLiteral("overlay_present"),
             restoreOverlay ? QStringLiteral("true")
                            : QStringLiteral("false")}
        });
    if (restoreOverlay) {
        QStringList labels;
        QStringList values;
        QStringList units;
        collectCurrentPrinterMetrics(&labels, &values, &units);
        updatePrinterOverlayInitialMetrics(labels, values, units);
        hydratePaseBadgeText(
            &printerOverlayConfig_, printerSystemMonitor_);

        PrinterProtocol::OperationContext context;
        QString errorMessage;
        if (!preparePrinterOperation(printerDevicePath_, generation,
                                     QString(), &context,
                                     &errorMessage)) {
            markPrinterSessionLost(
                tr("PASE overlay restoration was cancelled because the USB generation changed"),
                generation);
            return;
        }

        PrinterProtocol::MutationDetails mutationDetails;
        if (!printerProtocol_->configurePaseOverlay(
                printerDevicePath_, printerOverlayConfig_,
                &errorMessage, context, &mutationDetails)) {
            logPrinterLifecycleEvent(
                QStringLiteral("overlay_activation_completed"),
                generation,
                {
                    {QStringLiteral("outcome"),
                     QStringLiteral("failed")}
                });
            const QString failure = errorMessage.isEmpty()
                ? tr("PASE overlay restoration failed")
                : tr("PASE overlay restoration failed: %1")
                      .arg(errorMessage);
            markPrinterSessionLost(failure, generation);
            return;
        }
    }

    if (!printerGenerationIsCurrent(generation) ||
        generation != configuredPrinterGeneration_) {
        return;
    }
    logPrinterLifecycleEvent(
        QStringLiteral("overlay_activation_completed"), generation,
        {
            {QStringLiteral("outcome"),
             QStringLiteral("succeeded")},
            {QStringLiteral("overlay_present"),
             restoreOverlay ? QStringLiteral("true")
                            : QStringLiteral("false")}
        });
    transitionPrinterSessionState(
        PrinterSessionState::Active,
        QStringLiteral("display_session_active"));
    printerSessionRecoveryAttempt_ = 0;
    printerKeepaliveRetryCount_ = 0;
    printerOverlayLeaseRefreshNext_ = false;
    startPrinterMetrics();
    emit printerSessionStarted(generation);
    emit printerUploadProgress(
        restoreOverlay
            ? tr("PASE display session and overlay are active")
            : tr("PASE display session is active"),
        generation);
    restartPrinterKeepaliveAfterActivity();
    emit printerTransportReady(generation);
}

void DeviceWorker::markPrinterSessionLost(
    const QString &reason, quint64 generation) {
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ == PrinterSessionState::Lost) {
        return;
    }
    stopPrinterSession();
    transitionPrinterSessionState(
        PrinterSessionState::Lost,
        QStringLiteral("display_session_lost"));
    printerOverlayActivationPending_ = false;
    const QString message = reason.isEmpty()
        ? tr("The PASE display session is lost until a new USB endpoint generation appears")
        : reason;
    emit printerOperationError(message, generation);
    emit printerSessionLost(generation);
}

void DeviceWorker::sendKeepalive() {
    if (!device_ || !device_->is_connected()) {
        return;
    }
    device_->handshake();
}

void DeviceWorker::setRotation(int degrees) {
    if (!device_ || !device_->is_connected()) {
        return;
    }
    device_->set_rotation(degrees);
}

void DeviceWorker::rebootDevice() {
    // ADB reboot works, POST reboot doesn't
    std::system("adb -s $(adb devices 2>/dev/null | grep TRYX | cut -f1) reboot 2>/dev/null");
}

// --- DeviceManager ---

DeviceManager::DeviceManager(QObject *parent)
    : DeviceManager(new PrinterDeviceMonitor, true, parent) {}

DeviceManager::DeviceManager(bool remoteMode, QObject *parent)
    : QObject(parent), remoteMode_(remoteMode) {
    Q_ASSERT(remoteMode_);
    initializeRemote();
}

DeviceManager *DeviceManager::createRemote(QObject *parent) {
    return new DeviceManager(true, parent);
}

void DeviceManager::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerOverlayLeaseMode_ = mode;
    if (!worker_) {
        return;
    }
    if (worker_->thread() == QThread::currentThread()) {
        worker_->setPrinterOverlayLeaseMode(mode);
        return;
    }
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, mode]() {
            worker->setPrinterOverlayLeaseMode(mode);
        },
        Qt::QueuedConnection);
}

void DeviceManager::initializeRemote() {
    registerTryxRuntimeMetaTypes();

    QDBusConnection bus = QDBusConnection::sessionBus();
    remoteInterface_ = new QDBusInterface(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeInterfaceName(), bus, this);
    remoteOperationsInterface_ = new QDBusInterface(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus, this);
    remoteInterface_->setTimeout(2000);
    remoteOperationsInterface_->setTimeout(2000);
    remoteServiceWatcher_ = new QDBusServiceWatcher(
        tryxRuntimeServiceName(), bus,
        QDBusServiceWatcher::WatchForRegistration |
            QDBusServiceWatcher::WatchForUnregistration,
        this);
    connect(remoteServiceWatcher_, &QDBusServiceWatcher::serviceRegistered,
            this, &DeviceManager::handleRemoteServiceRegistered);
    connect(remoteServiceWatcher_, &QDBusServiceWatcher::serviceUnregistered,
            this, &DeviceManager::handleRemoteServiceUnregistered);

    const QString service = tryxRuntimeServiceName();
    const QString path = tryxRuntimeObjectPath();
    const QString interface = tryxRuntimeInterfaceName();
    bus.connect(service, path, interface, QStringLiteral("DeviceConnected"),
                this,
                SLOT(handleRemoteDeviceConnected(QString,QString,QString,QString,bool,bool,quint64)));
    bus.connect(service, path, interface, QStringLiteral("DeviceDisconnected"),
                this, SLOT(handleRemoteDeviceDisconnected(quint64)));
    bus.connect(service, path, interface, QStringLiteral("DeviceError"),
                this, SLOT(handleRemoteDeviceError(QString,quint64)));
    bus.connect(service, path, interface, QStringLiteral("BrightnessChanged"),
                this, SLOT(handleRemoteBrightnessChanged(int,quint64)));
    bus.connect(service, path, interface, QStringLiteral("ScreenConfigChanged"),
                this, SLOT(handleRemoteScreenConfigChanged(quint64)));
    bus.connect(service, path, interface, QStringLiteral("SysinfoSent"),
                this, SLOT(handleRemoteSysinfoSent(quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("PrinterTransportReady"),
                this, SLOT(handleRemotePrinterTransportReady(quint64)));
    bus.connect(service, path, interface, QStringLiteral("MediaUploaded"),
                this, SLOT(handleRemoteMediaUploaded(QString,quint64)));
    bus.connect(service, path, interface, QStringLiteral("MediaDeleted"),
                this, SLOT(handleRemoteMediaDeleted(quint64)));
    bus.connect(service, path, interface, QStringLiteral("MediaListUpdated"),
                this,
                SLOT(handleRemoteMediaListUpdated(QStringList,quint64)));
    bus.connect(service, path, interface, QStringLiteral("UploadStatus"),
                this, SLOT(handleRemoteUploadStatus(QString,quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("PrinterOperationsCancelled"), this,
                SLOT(handleRemotePrinterOperationsCancelled(quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("PrinterDeviceInfoReady"), this,
                SLOT(handleRemotePrinterDeviceInfoReady(TryxRuntimeDeviceInfo,quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("PrinterDeviceInfoFailed"), this,
                SLOT(handleRemotePrinterDeviceInfoFailed(QString,quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("PrinterPresenceChanged"), this,
                SLOT(handleRemotePrinterPresenceChanged(bool,bool,quint64)));
    bus.connect(service, path, interface,
                QStringLiteral("DisplaySessionChanged"), this,
                SLOT(handleRemoteDisplaySessionChanged(bool,quint64)));
    const QString operationsInterface = tryxRuntimeOperationsInterfaceName();
    bus.connect(service, path, operationsInterface,
                QStringLiteral("OperationChanged"), this,
                SLOT(handleRemoteOperationChanged(TryxRuntimeOperationInfo,quint64)));
    bus.connect(service, path, operationsInterface,
                QStringLiteral("OperationRemoved"), this,
                SLOT(handleRemoteOperationRemoved(QString,quint64)));
    bus.connect(service, path, operationsInterface,
                QStringLiteral("MediaCatalogUpdated"), this,
                SLOT(handleRemoteMediaCatalogUpdated(TryxRuntimeMediaCatalogSnapshot)));
    bus.connect(service, path, operationsInterface,
                QStringLiteral("MetricsStateUpdated"), this,
                SLOT(handleRemoteMetricsStateUpdated(TryxRuntimeMetricsState)));
    bus.connect(service, path, operationsInterface,
                QStringLiteral("DisplayStateUpdated"), this,
                SLOT(handleRemoteDisplayStateUpdated(TryxRuntimeDisplayState)));

    if (bus.interface() &&
        bus.interface()->isServiceRegistered(service)) {
        advanceRemoteServiceEpoch();
        requestRemoteApiCompatibility();
    }
}

void DeviceManager::requestRemoteApiCompatibility() {
    if (!remoteMode_ || !remoteOperationsInterface_) {
        return;
    }
    remoteApiCompatible_ = false;
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCall(
            QStringLiteral("GetRuntimeApiVersion")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<quint32> reply = *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    emit deviceError(
                        tr("The restarted TRYX runtime API could not be verified: %1")
                            .arg(reply.error().message()));
                    return;
                }
                if (reply.value() != tryxRuntimeApiVersion()) {
                    emit deviceError(
                        tr("The restarted TRYX runtime uses API %1, but this GUI requires API %2")
                            .arg(reply.value())
                            .arg(tryxRuntimeApiVersion()));
                    return;
                }
                remoteApiCompatible_ = true;
                requestRemoteSnapshot();
                requestRemoteMediaCatalog();
                requestRemoteOperationsSnapshot();
                requestRemoteMetricsState();
                requestRemoteDisplayState();
            });
}

void DeviceManager::requestRemoteMediaCatalog() {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        !remoteOperationsInterface_) {
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCall(
            QStringLiteral("GetMediaCatalog")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<TryxRuntimeMediaCatalogSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    if (reply.error().type() != QDBusError::UnknownMethod) {
                        emit deviceError(
                            tr("Failed to read the media catalog: %1")
                                .arg(reply.error().message()));
                    }
                    return;
                }
                handleRemoteMediaCatalogUpdated(reply.value());
            });
}

void DeviceManager::advanceRemoteServiceEpoch() {
    ++remoteServiceEpoch_;
    if (remoteServiceEpoch_ == 0) {
        ++remoteServiceEpoch_;
    }
}

void DeviceManager::requestRemoteSnapshot() {
    if (!remoteMode_ || !remoteApiCompatible_ || !remoteInterface_) {
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteInterface_->asyncCall(QStringLiteral("GetSnapshot")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<TryxRuntimeSnapshot> reply = *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    emit deviceError(
                        tr("Failed to read the TRYX background runtime state: %1")
                            .arg(reply.error().message()));
                    return;
                }
                applyRemoteSnapshot(reply.value());
            });
}

void DeviceManager::requestRemoteOperationsSnapshot() {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        !remoteOperationsInterface_) {
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCall(QStringLiteral("GetOperations")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<TryxRuntimeOperationsSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    emit deviceError(
                        tr("Failed to read background operation state: %1")
                            .arg(reply.error().message()));
                    return;
                }
                const TryxRuntimeOperationsSnapshot snapshot = reply.value();
                if (snapshot.revision < remoteOperationRevision_) {
                    return;
                }
                const QStringList previousIds = operationOrder_;
                operations_.clear();
                operationOrder_.clear();
                for (const TryxRuntimeOperationInfo &info :
                     snapshot.operations) {
                    OperationRecord record;
                    record.info = info;
                    operations_.insert(info.id, record);
                    operationOrder_.append(info.id);
                }
                activeOperationId_ = snapshot.activeOperationId;
                remoteOperationRevision_ = snapshot.revision;
                for (const QString &previousId : previousIds) {
                    if (!operations_.contains(previousId)) {
                        emit operationRemoved(previousId,
                                              remoteOperationRevision_);
                    }
                }
                for (const TryxRuntimeOperationInfo &info :
                     snapshot.operations) {
                    emit operationChanged(info, remoteOperationRevision_);
                }
                emit operationSnapshotUpdated(snapshot);
            });
}

void DeviceManager::requestRemoteMetricsState() {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        !remoteOperationsInterface_) {
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCall(
            QStringLiteral("GetMetricsState")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<TryxRuntimeMetricsState> reply =
                    *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    if (reply.error().type() != QDBusError::UnknownMethod) {
                        emit deviceError(
                            tr("Failed to read PASE metrics state: %1")
                                .arg(reply.error().message()));
                    }
                    return;
                }
                handleRemoteMetricsStateUpdated(reply.value());
            });
}

void DeviceManager::requestRemoteDisplayState() {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        !remoteOperationsInterface_) {
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCall(
            QStringLiteral("GetDisplayState")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceEpoch]() {
                const QDBusPendingReply<TryxRuntimeDisplayState> reply =
                    *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    emit deviceError(
                        tr("Failed to read PASE display state: %1")
                            .arg(reply.error().message()));
                    return;
                }
                handleRemoteDisplayStateUpdated(reply.value());
            });
}

void DeviceManager::applyRemoteSnapshot(
    const TryxRuntimeSnapshot &snapshot) {
    if (snapshot.revision < remoteRevision_) {
        return;
    }

    const bool wasConnected = connected_;
    const bool oldPresence = remotePrinterClassDevicePresent_;
    const bool oldSession = printerDisplaySessionActive_;
    remoteRevision_ = snapshot.revision;
    connected_ = snapshot.connected;
    printerClassConnected_ = snapshot.printerClassConnected;
    remotePrinterClassDevicePresent_ =
        snapshot.printerClassDevicePresent;
    printerDisplaySessionActive_ = snapshot.displaySessionActive;
    if (!remotePrinterClassDevicePresent_) {
        remoteTypedMediaCatalogAvailable_ = false;
    } else if (!oldPresence) {
        requestRemoteMediaCatalog();
    }

    if (connected_) {
        emit deviceConnected(snapshot.productId, snapshot.serial,
                             snapshot.firmware, snapshot.appVersion);
    } else if (wasConnected) {
        emit deviceDisconnected();
    }
    if (oldPresence != remotePrinterClassDevicePresent_) {
        emit printerPresenceChanged(remotePrinterClassDevicePresent_);
    }
    if (oldSession != printerDisplaySessionActive_) {
        emit printerDisplaySessionChanged(printerDisplaySessionActive_);
    }
    if (!hasTypedMediaCatalog()) {
        emit mediaListUpdated(snapshot.mediaFiles);
    }
    if (!snapshot.diagnostic.isEmpty()) {
        emit uploadStatus(snapshot.diagnostic);
    }
}

bool DeviceManager::acceptRemoteRevision(quint64 revision) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        revision <= remoteRevision_) {
        return false;
    }
    remoteRevision_ = revision;
    return true;
}

void DeviceManager::remoteCall(const QString &method,
                               const QVariantList &arguments) {
    if (!remoteMode_ || !remoteInterface_) {
        return;
    }
    if (!remoteApiCompatible_) {
        emit deviceError(
            tr("Background runtime call %1 was blocked until its API is verified")
                .arg(method));
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteInterface_->asyncCallWithArgumentList(method, arguments), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, method, watcher, serviceEpoch]() {
                const QDBusPendingReply<> reply = *watcher;
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.isError()) {
                    emit deviceError(
                        tr("Background runtime call %1 failed: %2")
                            .arg(method, reply.error().message()));
                }
            });
}

void DeviceManager::remoteOperationCall(const QString &method,
                                        const QVariantList &arguments) {
    if (!remoteMode_ || !remoteOperationsInterface_) {
        return;
    }
    QString operationId;
    if ((method == QStringLiteral("QueueUpload") ||
         method == QStringLiteral("QueueUploadWithApply") ||
         method == QStringLiteral("QueueEnsureMediaAndApply") ||
         method == QStringLiteral("QueueDeleteMedia") ||
         method == QStringLiteral("QueueApply") ||
         method == QStringLiteral("QueueApplyWithMetrics") ||
         method == QStringLiteral("QueueMetricsConfig")) &&
        !arguments.isEmpty()) {
        operationId = arguments.first().toString();
    } else if (method == QStringLiteral("RetryOperation") &&
               arguments.size() > 1) {
        operationId = arguments.at(1).toString();
    }
    if (!remoteApiCompatible_) {
        const QString message =
            tr("Background operation %1 was blocked until the runtime API is verified")
                .arg(method);
        if (!operationId.isEmpty()) {
            failRemoteOperationRequest(operationId, message);
        }
        emit deviceError(message);
        return;
    }
    const quint64 serviceEpoch = remoteServiceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        remoteOperationsInterface_->asyncCallWithArgumentList(method,
                                                               arguments),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, method, operationId, watcher, serviceEpoch]() {
                const QDBusMessage reply = watcher->reply();
                watcher->deleteLater();
                if (serviceEpoch != remoteServiceEpoch_) {
                    return;
                }
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    const QString message =
                        tr("Background operation call %1 failed: %2")
                            .arg(method, reply.errorMessage());
                    if (!operationId.isEmpty()) {
                        failRemoteOperationRequest(operationId, message);
                    }
                    emit deviceError(message);
                }
            });
}

void DeviceManager::trackRemoteOperationRequest(
    const TryxRuntimeOperationInfo &source) {
    if (!remoteMode_ || source.id.isEmpty() ||
        operations_.contains(source.id)) {
        return;
    }
    OperationRecord record;
    record.info = source;
    record.info.state = QStringLiteral("Queued");
    record.info.stage = QStringLiteral("Dispatching");
    record.info.message = tr("Submitting operation to the background runtime...");
    operations_.insert(record.info.id, record);
    operationOrder_.append(record.info.id);
    activeOperationId_ = record.info.id;
    emit operationChanged(record.info, remoteOperationRevision_);
}

void DeviceManager::failRemoteOperationRequest(
    const QString &operationId, const QString &message) {
    if (!remoteMode_ || operationId.isEmpty()) {
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        OperationRecord record;
        record.info.id = operationId;
        operations_.insert(operationId, record);
        operationOrder_.append(operationId);
        found = operations_.find(operationId);
    }
    if (operationIsTerminal(found->info.state)) {
        return;
    }
    found->info.state = QStringLiteral("Failed");
    found->info.stage = QStringLiteral("Failed");
    found->info.errorCategory = QStringLiteral("RuntimeUnavailable");
    found->info.retryMode.clear();
    found->info.message = message;
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    emit operationChanged(found->info, remoteOperationRevision_);
    emit operationSnapshotUpdated(operationSnapshot());
}

void DeviceManager::handleRemoteDeviceConnected(
    const QString &productId, const QString &serial,
    const QString &firmware, const QString &appVersion,
    bool printerClassConnected, bool printerClassDevicePresent,
    quint64 revision) {
    if (!acceptRemoteRevision(revision)) {
        return;
    }
    connected_ = true;
    printerClassConnected_ = printerClassConnected;
    remotePrinterClassDevicePresent_ = printerClassDevicePresent;
    if (!printerClassDevicePresent) {
        remoteTypedMediaCatalogAvailable_ = false;
    } else {
        requestRemoteMediaCatalog();
    }
    emit deviceConnected(productId, serial, firmware, appVersion);
}

void DeviceManager::handleRemoteDeviceDisconnected(quint64 revision) {
    if (!acceptRemoteRevision(revision)) {
        return;
    }
    const bool notify = connected_;
    connected_ = false;
    printerClassConnected_ = false;
    printerDisplaySessionActive_ = false;
    remotePrinterClassDevicePresent_ = false;
    remoteTypedMediaCatalogAvailable_ = false;
    if (notify) {
        emit deviceDisconnected();
    }
}

void DeviceManager::handleRemoteDeviceError(const QString &message,
                                            quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit deviceError(message);
    }
}

void DeviceManager::handleRemoteBrightnessChanged(int value,
                                                  quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit brightnessChanged(value);
    }
}

void DeviceManager::handleRemoteScreenConfigChanged(quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit screenConfigChanged();
    }
}

void DeviceManager::handleRemoteSysinfoSent(quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit sysinfoSent();
    }
}

void DeviceManager::handleRemotePrinterTransportReady(
    quint64 revision) {
    if (acceptRemoteRevision(revision) &&
        remotePrinterClassDevicePresent_ &&
        printerDisplaySessionActive_ &&
        activeOperationId_.isEmpty()) {
        emit printerTransportReady();
    }
}

void DeviceManager::handleRemoteMediaUploaded(const QString &filename,
                                              quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit mediaUploaded(filename);
    }
}

void DeviceManager::handleRemoteMediaDeleted(quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit mediaDeleted();
    }
}

void DeviceManager::handleRemoteMediaListUpdated(const QStringList &files,
                                                 quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        if (!hasTypedMediaCatalog()) {
            emit mediaListUpdated(files);
        }
    }
}

void DeviceManager::handleRemoteMediaCatalogUpdated(
    const TryxRuntimeMediaCatalogSnapshot &snapshot) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        (remoteTypedMediaCatalogAvailable_ &&
         snapshot.revision <= remoteCatalogRevision_)) {
        return;
    }
    remoteTypedMediaCatalogAvailable_ =
        remotePrinterClassDevicePresent_ || printerClassConnected_;
    remoteCatalogRevision_ = snapshot.revision;
    mediaCatalog_ = snapshot;
    QStringList files;
    QSet<QString> seen;
    for (const TryxRuntimeMediaEntry &entry : snapshot.entries) {
        if (!seen.contains(entry.name)) {
            seen.insert(entry.name);
            files.append(entry.name);
        }
    }
    if (remoteTypedMediaCatalogAvailable_) {
        emit mediaCatalogUpdated(mediaCatalog_);
        emit mediaListUpdated(files);
    }
}

void DeviceManager::handleRemoteUploadStatus(const QString &status,
                                             quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit uploadStatus(status);
    }
}

void DeviceManager::handleRemotePrinterOperationsCancelled(
    quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit printerOperationsCancelled();
    }
}

void DeviceManager::handleRemotePrinterDeviceInfoReady(
    const TryxRuntimeDeviceInfo &source, quint64 revision) {
    if (!acceptRemoteRevision(revision)) {
        return;
    }
    PrinterProtocol::DeviceInfo info;
    info.devicePath = source.devicePath;
    info.manufacturer = source.manufacturer;
    info.usbProduct = source.usbProduct;
    info.usbSerial = source.usbSerial;
    info.osName = source.osName;
    info.osVersion = source.osVersion;
    info.firmwareVersion = source.firmwareVersion;
    info.productName = source.productName;
    info.appVersion = source.appVersion;
    info.serialNumber = source.serialNumber;
    info.chipId = source.chipId;
    info.serialNumberLocked = source.serialNumberLocked;
    emit printerDeviceInfoReady(info);
}

void DeviceManager::handleRemotePrinterDeviceInfoFailed(
    const QString &message, quint64 revision) {
    if (acceptRemoteRevision(revision)) {
        emit printerDeviceInfoFailed(message);
    }
}

void DeviceManager::handleRemotePrinterPresenceChanged(
    bool present, bool printerClassConnected, quint64 revision) {
    if (!acceptRemoteRevision(revision)) {
        return;
    }
    remotePrinterClassDevicePresent_ = present;
    printerClassConnected_ = printerClassConnected;
    if (!present) {
        remoteTypedMediaCatalogAvailable_ = false;
    } else {
        requestRemoteMediaCatalog();
    }
    emit printerPresenceChanged(present);
}

void DeviceManager::handleRemoteDisplaySessionChanged(bool active,
                                                      quint64 revision) {
    if (!acceptRemoteRevision(revision)) {
        return;
    }
    printerDisplaySessionActive_ = active;
    emit printerDisplaySessionChanged(active);
}

void DeviceManager::handleRemoteOperationChanged(
    const TryxRuntimeOperationInfo &info, quint64 revision) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        revision <= remoteOperationRevision_ ||
        info.id.isEmpty()) {
        return;
    }
    remoteOperationRevision_ = revision;
    if (!operations_.contains(info.id)) {
        operationOrder_.append(info.id);
    }
    OperationRecord record;
    record.info = info;
    operations_.insert(info.id, record);
    if (operationIsTerminal(info.state)) {
        if (activeOperationId_ == info.id) {
            activeOperationId_.clear();
        }
    } else {
        activeOperationId_ = info.id;
    }
    emit operationChanged(info, revision);
}

void DeviceManager::handleRemoteOperationRemoved(
    const QString &operationId, quint64 revision) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        revision <= remoteOperationRevision_) {
        return;
    }
    remoteOperationRevision_ = revision;
    operations_.remove(operationId);
    operationOrder_.removeAll(operationId);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    emit operationRemoved(operationId, revision);
}

void DeviceManager::handleRemoteMetricsStateUpdated(
    const TryxRuntimeMetricsState &state) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        state.revision < remoteMetricsRevision_) {
        return;
    }
    remoteMetricsRevision_ = state.revision;
    metricsState_ = state;
    emit metricsStateUpdated(metricsState_);
}

void DeviceManager::handleRemoteDisplayStateUpdated(
    const TryxRuntimeDisplayState &state) {
    if (!remoteMode_ || !remoteApiCompatible_ ||
        (remoteDisplayRevisionReceived_ &&
         state.revision <= remoteDisplayRevision_)) {
        return;
    }
    remoteDisplayRevision_ = state.revision;
    remoteDisplayRevisionReceived_ = true;
    const int previousBrightness = displayState_.brightness;
    const bool hadValidState = displayState_.valid;
    displayState_ = state;
    emit displayStateUpdated(displayState_);
    if (displayState_.valid &&
        (!hadValidState ||
         previousBrightness != displayState_.brightness)) {
        emit brightnessChanged(displayState_.brightness);
    }
}

void DeviceManager::handleRemoteServiceRegistered(const QString &service) {
    Q_UNUSED(service);
    advanceRemoteServiceEpoch();
    remoteRevision_ = 0;
    remoteOperationRevision_ = 0;
    remoteCatalogRevision_ = 0;
    remoteMetricsRevision_ = 0;
    remoteDisplayRevision_ = 0;
    remoteDisplayRevisionReceived_ = false;
    remoteApiCompatible_ = false;
    remoteTypedMediaCatalogAvailable_ = false;
    requestRemoteApiCompatibility();
}

void DeviceManager::handleRemoteServiceUnregistered(const QString &service) {
    Q_UNUSED(service);
    const bool wasConnected = connected_;
    const bool hadPresence = remotePrinterClassDevicePresent_;
    const bool hadSession = printerDisplaySessionActive_;
    advanceRemoteServiceEpoch();
    remoteRevision_ = 0;
    remoteOperationRevision_ = 0;
    remoteCatalogRevision_ = 0;
    remoteMetricsRevision_ = 0;
    remoteDisplayRevision_ = 0;
    remoteDisplayRevisionReceived_ = false;
    remoteApiCompatible_ = false;
    remoteTypedMediaCatalogAvailable_ = false;
    connected_ = false;
    printerClassConnected_ = false;
    remotePrinterClassDevicePresent_ = false;
    printerDisplaySessionActive_ = false;
    mediaCatalog_ = TryxRuntimeMediaCatalogSnapshot{};
    emit mediaCatalogUpdated(mediaCatalog_);
    const QString runtimeStopped = tr("TRYX background runtime stopped");
    metricsState_ = TryxRuntimeMetricsState{};
    metricsState_.diagnostic = runtimeStopped;
    emit metricsStateUpdated(metricsState_);
    displayState_ = TryxRuntimeDisplayState{};
    displayState_.diagnostic = runtimeStopped;
    emit displayStateUpdated(displayState_);
    const QStringList oldOperationIds = operationOrder_;
    for (const QString &operationId : oldOperationIds) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd() &&
            !operationIsTerminal(found->info.state)) {
            failRemoteOperationRequest(operationId, runtimeStopped);
        }
    }
    activeOperationId_.clear();
    emit operationSnapshotUpdated(operationSnapshot());
    if (wasConnected) {
        emit deviceDisconnected();
    }
    if (hadPresence) {
        emit printerPresenceChanged(false);
    }
    if (hadSession) {
        emit printerDisplaySessionChanged(false);
    }
    emit deviceError(runtimeStopped);
}

DeviceManager::DeviceManager(PrinterDeviceMonitor *printerMonitor,
                             bool startPrinterMonitor, QObject *parent)
    : QObject(parent),
      worker_(new DeviceWorker),
      printerMediaPreparer_(new PrinterMediaPreparer),
      keepaliveTimer_(new QTimer(this)),
      printerMonitor_(printerMonitor) {
    automaticPrinterSessionStart_ = startPrinterMonitor;
    printerMonitor_->setParent(this);
    qRegisterMetaType<PrinterProtocol::UsbPrinterDevice>();
    qRegisterMetaType<PrinterProtocol::DiscoverySnapshot>();
    qRegisterMetaType<PrinterProtocol::DeviceInfo>();
    qRegisterMetaType<PrinterProtocol::MediaFile>();
    qRegisterMetaType<QList<PrinterProtocol::MediaFile>>();
    qRegisterMetaType<PrinterProtocol::MutationOutcome>();
    qRegisterMetaType<PrinterProtocol::PaseOverlayConfig>();
    qRegisterMetaType<PrinterProtocol::PaseDisplayState>();
    qRegisterMetaType<TryxRuntimeOperationInfo>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayState>();

    QString overlayLeaseConfigStatus =
        QStringLiteral("test-default");
    if (startPrinterMonitor) {
        std::error_code configPathError;
        const bool configFileExists = std::filesystem::exists(
            panorama::ConfigManager::get_config_path(),
            configPathError);
        const auto config = panorama::ConfigManager::load_config();
        if (!config) {
            overlayLeaseConfigStatus =
                QStringLiteral("unreadable-or-invalid");
            qWarning().noquote()
                << "Invalid TRYX config.json; using"
                << printerOverlayLeaseModeName(
                       PrinterOverlayLeaseMode::
                           PingAndOverlayLease)
                << "for the PASE overlay lease mode";
        } else if (config->pase_overlay_lease_mode ==
                   "ping-only") {
            printerOverlayLeaseMode_ =
                PrinterOverlayLeaseMode::PingOnly;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        } else {
            printerOverlayLeaseMode_ =
                PrinterOverlayLeaseMode::PingAndOverlayLease;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        }
    }
    setPrinterOverlayLeaseMode(printerOverlayLeaseMode_);
    if (startPrinterMonitor) {
        logPrinterLifecycleEvent(
            QStringLiteral("overlay_lease_mode_selected"),
            printerGeneration_,
            {
                {QStringLiteral("lease_mode"),
                 printerOverlayLeaseModeName(
                     printerOverlayLeaseMode_)},
                {QStringLiteral("config_status"),
                 overlayLeaseConfigStatus}
            });
    }

    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    printerMediaPreparer_->moveToThread(&printerPreparationThread_);
    connect(&printerPreparationThread_, &QThread::finished,
            printerMediaPreparer_, &QObject::deleteLater);

    connect(worker_, &DeviceWorker::connected, this,
            [this](const QString &pid, const QString &serial,
                   const QString &fw, const QString &app) {
                if (printerSnapshot_.blocksLegacyTransport()) {
                    emit requestDisconnect();
                    return;
                }
                connected_ = true;
                printerClassConnected_ = false;
                emit deviceConnected(pid, serial, fw, app);
            });
    connect(worker_, &DeviceWorker::disconnected, this, [this]() {
        if (printerClassConnected_) {
            return;
        }
        connected_ = false;
        emit deviceDisconnected();
    });
    const auto legacyResultIsCurrent = [this]() {
        return connected_ && !printerClassConnected_ &&
               !printerSnapshot_.blocksLegacyTransport();
    };
    connect(worker_, &DeviceWorker::error, this, [this](const QString &message) {
        if (!printerSnapshot_.blocksLegacyTransport()) {
            emit deviceError(message);
        }
    });
    connect(worker_, &DeviceWorker::brightnessSet, this,
            [this, legacyResultIsCurrent](int value) {
                if (legacyResultIsCurrent()) {
                    emit brightnessChanged(value);
                }
            });
    connect(worker_, &DeviceWorker::screenConfigSet, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit screenConfigChanged();
                }
            });
    connect(worker_, &DeviceWorker::mediaUploaded, this,
            [this, legacyResultIsCurrent](const QString &fileName) {
                if (legacyResultIsCurrent()) {
                    emit mediaUploaded(fileName);
                }
            });
    connect(worker_, &DeviceWorker::mediaDeleted, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit mediaDeleted();
                }
            });
    connect(worker_, &DeviceWorker::mediaListReady, this,
            [this, legacyResultIsCurrent](const QStringList &files) {
                if (legacyResultIsCurrent()) {
                    emit mediaListUpdated(files);
                }
            });
    connect(worker_, &DeviceWorker::uploadProgress, this,
            [this, legacyResultIsCurrent](const QString &status) {
                if (legacyResultIsCurrent()) {
                    emit uploadStatus(status);
                }
            });

    const auto printerResultIsCurrent = [this](quint64 generation) {
        return generation == printerGeneration_ && printerClassConnected_ &&
               printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready;
    };
    const auto printerOperationResultIsExpected =
        [this, printerResultIsCurrent](const QString &operationId,
                                       quint64 generation) {
            const auto found = operations_.constFind(operationId);
            if (operationId.isEmpty() ||
                activeOperationId_ != operationId ||
                found == operations_.constEnd() ||
                found->info.deviceGeneration != generation) {
                return false;
            }
            return printerResultIsCurrent(generation) ||
                   found->deviceChangePending;
        };
    connect(worker_, &DeviceWorker::printerOperationError, this,
            [this, printerResultIsCurrent](const QString &message, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit deviceError(message);
                }
            });
    connect(worker_, &DeviceWorker::printerUploadProgress, this,
            [this, printerResultIsCurrent](const QString &status, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit uploadStatus(status);
                }
            });
    connect(worker_, &DeviceWorker::printerSessionStarted, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                if (printerRecoveryRequired_) {
                    setPrinterDisplaySessionActive(false);
                    emit uploadStatus(printerMutationUnavailableStatusText());
                    return;
                }
                const QString sysfsPath =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().sysfsPath
                        : QString();
                logPrinterLifecycleEvent(
                    QStringLiteral("recovery_completed"), generation,
                    {
                        {QStringLiteral("sysfs_path"), sysfsPath},
                        {QStringLiteral("serial"),
                         printerDeviceSerial_.trimmed()},
                        {QStringLiteral("disconnect_count"),
                         QString::number(printerDisconnectCount_)},
                        {QStringLiteral("elapsed_ms"),
                         printerGenerationElapsedTimer_.isValid()
                             ? QString::number(
                                   printerGenerationElapsedTimer_
                                       .elapsed())
                             : QStringLiteral("-1")},
                        {QStringLiteral("lease_mode"),
                         printerOverlayLeaseModeName(
                             printerOverlayLeaseMode_)}
                    });
                printerDisplaySessionLost_ = false;
                printerSessionLossRemovalObserved_ = false;
                setPrinterDisplaySessionActive(true);
                printerSessionResumePending_ = false;
                printerSessionResumeSerial_.clear();
                if ((!displayState_.valid ||
                     displayState_.deviceSerial !=
                         printerDeviceSerial_.trimmed()) &&
                    displayStateReadGeneration_ != generation) {
                    displayStateReadGeneration_ = generation;
                    emit requestPrinterDisplayState(
                        currentPrinterPath(), printerGeneration_);
                }
                if (!activeOperationId_.isEmpty() &&
                    operations_.contains(activeOperationId_)) {
                    OperationRecord &record =
                        operations_[activeOperationId_];
                    if (record.uploadFinalizationReconciliationPending &&
                        record.info.stage ==
                            QStringLiteral("RecoveringFinalization")) {
                        const QString currentDeviceIdentity =
                            printerDeviceSerial_.trimmed();
                        if (record.uploadDeviceIdentity.isEmpty() ||
                            record.uploadDeviceIdentity !=
                                currentDeviceIdentity) {
                            const QString operationId =
                                activeOperationId_;
                            record.uploadFinalizationReconciliationPending =
                                false;
                            record.requiresDeviceRecovery = true;
                            record.retryMustUseNewRemoteName = true;
                            requirePrinterRecovery(tr(
                                "PASE reconnected with an unverified device identity after the final upload acknowledgement was lost. Power-cycle the device before a manual retry."));
                            handlePreparedUploadFailure(
                                operationId,
                                tr("The completed upload could not be reconciled safely because the USB device identity changed"),
                                PrinterProtocol::MutationOutcome::
                                    PartialOrUnknown);
                            emit printerOperationsCancelled();
                            return;
                        }
                        record.info.deviceGeneration =
                            printerGeneration_;
                        record.deviceChangePending = false;
                        record.deviceChangeMessage.clear();
                        record.info.state = QStringLiteral("Refreshing");
                        record.info.stage =
                            QStringLiteral("RefreshingMedia");
                        record.info.message = tr(
                            "The final upload acknowledgement was lost; verifying FileList without retransmitting media...");
                        publishOperation(activeOperationId_);
                        emit requestPrinterRefreshMedia(
                            currentPrinterPath(), activeOperationId_,
                            printerGeneration_);
                    }
                }
                const bool samplingActive = metricsState_.enabled &&
                    !metricsState_.metrics.isEmpty();
                if (metricsState_.samplingActive != samplingActive) {
                    metricsState_.samplingActive = samplingActive;
                    publishMetricsState();
                }
                resumePendingDeleteReconciliation();
            });
    connect(worker_, &DeviceWorker::printerSessionStopped, this,
            [this](quint64 generation) {
                if (generation == printerGeneration_) {
                    setPrinterDisplaySessionActive(false);
                    printerSessionResumePending_ = false;
                    printerSessionResumeSerial_.clear();
                    if (metricsState_.samplingActive) {
                        metricsState_.samplingActive = false;
                        publishMetricsState();
                    }
                }
            });
    connect(worker_, &DeviceWorker::printerSessionLost, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                if (!activeOperationId_.isEmpty() &&
                    operations_.contains(activeOperationId_)) {
                    const QString operationId = activeOperationId_;
                    OperationRecord &record = operations_[operationId];
                    if (record.uploadFinalizationReconciliationPending) {
                        record.uploadFinalizationReconciliationPending =
                            false;
                        record.requiresDeviceRecovery = true;
                        record.retryMustUseNewRemoteName = true;
                        requirePrinterRecovery(tr(
                            "PASE did not recover far enough to verify the committed upload. Power-cycle the device before a manual retry."));
                        handlePreparedUploadFailure(
                            operationId,
                            tr("The final upload acknowledgement was lost and the read-only FileList reconciliation could not start"),
                            PrinterProtocol::MutationOutcome::PartialOrUnknown);
                        emit printerOperationsCancelled();
                        return;
                    }
                }
                if (!printerDisplaySessionLost_) {
                    printerSessionLossRemovalObserved_ = false;
                }
                printerDisplaySessionLost_ = true;
                setPrinterDisplaySessionActive(false);
                printerSessionResumePending_ = false;
                printerSessionResumeSerial_.clear();
                if (metricsState_.samplingActive) {
                    metricsState_.samplingActive = false;
                    publishMetricsState();
                }
                cancelForegroundForGenerationChange(
                    tr("PASE display session was lost"));
                emit printerOperationsCancelled();
                emit uploadStatus(
                    tr("PASE display session is lost; waiting for a new USB endpoint generation"));
            });
    connect(worker_, &DeviceWorker::printerForegroundProgress, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &stage,
                                           qint64 completed, qint64 total,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.stage = stage;
                record.info.state =
                    stage == QStringLiteral("Beginning")
                        ? QStringLiteral("Beginning")
                        : stage == QStringLiteral("Transferring")
                            ? QStringLiteral("Transferring")
                            : stage == QStringLiteral("Ending")
                                ? QStringLiteral("Ending")
                                : stage == QStringLiteral("Applying")
                                    ? QStringLiteral("Applying")
                                    : QStringLiteral("Preflight");
                record.info.completed = completed;
                record.info.total = total;
                if (stage == QStringLiteral("Transferring") &&
                    completed >= record.info.confirmedBytes) {
                    record.info.confirmedBytes = completed;
                    record.info.lastConfirmedChunkIndex =
                        completed > 0
                            ? (completed - 1) / kFileTransmitChunkSize
                            : -1;
                }
                record.info.message = message;
                publishOperation(operationId);
            });
    connect(worker_, &DeviceWorker::printerUploadFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, const QString &uploadPath,
                const QString &remoteName, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.preparedPath = uploadPath;
                record.remoteName = remoteName;
                if (record.originalRemoteName.isEmpty()) {
                    record.originalRemoteName = remoteName;
                }
                record.info.resultName = remoteName;
                if (!success) {
                    const QString outcomeName = mutationOutcomeName(outcome);
                    if (record.info.terminalOutcome.isEmpty()) {
                        record.info.terminalOutcome = outcomeName;
                    }
                    if (record.info.primaryErrorCategory.isEmpty()) {
                        record.info.primaryErrorCategory = outcomeName;
                    }
                    if (record.info.primaryErrorMessage.isEmpty()) {
                        record.info.primaryErrorMessage = errorMessage;
                    }
                    if (outcome ==
                        PrinterProtocol::MutationOutcome::
                            FinalizationUnknown) {
                        record.info.confirmedBytes =
                            qMax(record.info.confirmedBytes,
                                 record.info.total);
                        record.info.lastConfirmedChunkIndex =
                            record.info.confirmedBytes > 0
                                ? (record.info.confirmedBytes - 1) /
                                      kFileTransmitChunkSize
                                : -1;
                        if (record.uploadDeviceIdentity.isEmpty()) {
                            record.uploadDeviceIdentity =
                                printerDeviceSerial_.trimmed();
                        }
                        record.uploadFinalizationReconciliationPending =
                            true;
                        record.info.state = QStringLiteral("Refreshing");
                        record.info.stage =
                            QStringLiteral("RecoveringFinalization");
                        record.info.message = tr(
                            "All media data was acknowledged, but the final status was lost. Recovering the session to verify FileList without retransmission...");
                        QString cacheError;
                        if (!writeRetryCache(
                                operationId,
                                QStringLiteral("FinalizationUnknown"),
                                &cacheError)) {
                            qWarning().noquote()
                                << tr("Cannot persist pending upload finalization reconciliation: %1")
                                       .arg(cacheError);
                        }
                        publishOperation(operationId);
                        return;
                    }
                    if (outcome ==
                        PrinterProtocol::MutationOutcome::PartialOrUnknown) {
                        record.requiresDeviceRecovery = true;
                        record.retryMustUseNewRemoteName = true;
                        requirePrinterRecovery(tr(
                            "The PASE transfer ended in an unknown partial state. Power-cycle the device before Retry or Save; the prepared media has been preserved."));
                    }
                    handlePreparedUploadFailure(operationId, errorMessage,
                                                outcome);
                    return;
                }
                if (record.deviceChangePending) {
                    handlePreparedUploadFailure(
                        operationId,
                        record.deviceChangeMessage.isEmpty()
                            ? tr("USB changed before uploaded media could be verified")
                            : record.deviceChangeMessage,
                        PrinterProtocol::MutationOutcome::PartialOrUnknown);
                    return;
                }
                if (record.cancelRequested) {
                    worker_->clearPrinterOperationCancellation(operationId);
                }
                record.info.state = QStringLiteral("Refreshing");
                record.info.stage = QStringLiteral("RefreshingMedia");
                record.info.message =
                    tr("Upload acknowledged; verifying the device file list...");
                publishOperation(operationId);
                emit requestPrinterRefreshMedia(currentPrinterPath(),
                                                operationId,
                                                printerGeneration_);
            });
    connect(worker_, &DeviceWorker::printerMediaListReady, this,
            [this, printerResultIsCurrent,
             printerOperationResultIsExpected](const QString &operationId,
                                           const QList<PrinterProtocol::MediaFile> &mediaFiles,
                                           quint64 generation) {
                QStringList files;
                QSet<QString> seenNames;
                for (const PrinterProtocol::MediaFile &media : mediaFiles) {
                    if (!seenNames.contains(media.name)) {
                        seenNames.insert(media.name);
                        files.append(media.name);
                    }
                }
                if (operationId.isEmpty()) {
                    if (printerResultIsCurrent(generation)) {
                        updateMediaCatalog(mediaFiles);
                        emit mediaListUpdated(files);
                    }
                    return;
                }
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (!printerResultIsCurrent(generation)) {
                    handlePreparedUploadFailure(
                        operationId,
                        record.deviceChangeMessage.isEmpty()
                            ? tr("USB changed before FileList could be reconciled")
                            : record.deviceChangeMessage,
                        record.retryPreflight
                            ? PrinterProtocol::MutationOutcome::NotStarted
                            : PrinterProtocol::MutationOutcome::PartialOrUnknown);
                    return;
                }
                if (record.originLookupPending) {
                    record.originLookupPending = false;
                    updateMediaCatalog(mediaFiles);
                    emit mediaListUpdated(files);
                    if (record.cancelRequested) {
                        finishOperation(
                            operationId, QStringLiteral("Cancelled"),
                            QStringLiteral("UserCancelled"), QString(),
                            tr("Operation cancelled by the user"));
                        return;
                    }
                    const QString reusableName = findReusableMediaOrigin(
                        record.sourceContentSha256,
                        record.conversionProfile, mediaFiles);
                    if (!reusableName.isEmpty()) {
                        record.remoteName = reusableName;
                        record.mediaFile = reusableName;
                        record.info.resultName = reusableName;
                        record.info.state = QStringLiteral("Applying");
                        record.info.stage = QStringLiteral("ReusingExisting");
                        record.info.message = tr(
                            "The media is already on the device; applying it without conversion or upload...");
                        publishOperation(operationId);
                        emit requestPrinterApplyMedia(
                            currentPrinterPath(), record.mediaFile,
                            record.applyRequest, record.updateMetrics,
                            operationId, printerGeneration_);
                        return;
                    }
                    record.info.state = QStringLiteral("Converting");
                    record.info.stage = QStringLiteral("Converting");
                    record.info.message = tr(
                        "No confirmed existing copy was found; preparing media for upload...");
                    publishOperation(operationId);
                    emit requestEndPrinterForegroundOperation(operationId,
                                                              generation);
                    emit requestPreparePrinterMedia(
                        operationId, currentPrinterPath(),
                        record.sourcePath, record.sourceContentSha256,
                        printerGeneration_);
                    return;
                }
                const QFileInfo preparedInfo(record.preparedPath);
                const qint64 preparedSize = preparedInfo.size();
                const auto exactMedia = std::find_if(
                    mediaFiles.cbegin(), mediaFiles.cend(),
                    [&record, preparedSize](
                        const PrinterProtocol::MediaFile &media) {
                        return media.name == record.remoteName &&
                               media.source ==
                                   PrinterProtocol::MediaSource::User &&
                               !media.readOnly &&
                               static_cast<qint64>(media.size) == preparedSize;
                    });
                const bool exactPreparedFilePresent =
                    preparedInfo.exists() && preparedInfo.isFile() &&
                    preparedSize > 0 && exactMedia != mediaFiles.cend();
                const bool retryRequiresFreshTarget =
                    record.retryPreflight &&
                    record.retryMustUseNewRemoteName;
                const bool exactPreparedFileCanBeTrusted =
                    exactPreparedFilePresent &&
                    !retryRequiresFreshTarget;
                const bool finalizationReconciliation =
                    record.uploadFinalizationReconciliationPending;
                if (exactPreparedFileCanBeTrusted &&
                    finalizationReconciliation) {
                    record.uploadFinalizationReconciliationPending = false;
                }
                bool thumbnailPromotionFailed = false;
                if (exactPreparedFileCanBeTrusted) {
                    TryxRuntimeMediaEntry verifiedEntry;
                    verifiedEntry.name = exactMedia->name;
                    verifiedEntry.size = exactMedia->size;
                    verifiedEntry.source = 1U;
                    verifiedEntry.readOnly = exactMedia->readOnly;
                    const bool hasStagedThumbnail =
                        !record.stagedThumbnailPath.isEmpty();
                    const QString thumbnailKey =
                        promoteThumbnailForOperation(operationId,
                                                     verifiedEntry);
                    thumbnailPromotionFailed =
                        hasStagedThumbnail && thumbnailKey.isEmpty();
                }
                updateMediaCatalog(mediaFiles);
                emit mediaListUpdated(files);
                if (thumbnailPromotionFailed) {
                    record.info.confirmedBytes = 0;
                    record.info.lastConfirmedChunkIndex = -1;
                    handlePreparedUploadFailure(
                        operationId,
                        tr("The media is present on the device, but its preview could not be committed. Retry will publish the preview without retransmitting the media"),
                        PrinterProtocol::MutationOutcome::NotStarted);
                    return;
                }
                if (record.retryPreflight) {
                    record.retryPreflight = false;
                    if (record.cancelRequested) {
                        handlePreparedUploadFailure(
                            operationId,
                            tr("Operation cancelled by the user"),
                            PrinterProtocol::MutationOutcome::Cancelled);
                        return;
                    }
                    if (record.retryMustUseNewRemoteName) {
                        const QString previousRemoteName =
                            record.remoteName;
                        const QString nameForSuffix =
                            record.originalRemoteName.isEmpty()
                                ? record.remoteName
                                : record.originalRemoteName;
                        const QString originalSuffix =
                            nameForSuffix.contains(
                                QStringLiteral(".png.h264_"))
                                ? QStringLiteral("png")
                                : nameForSuffix.contains(
                                      QStringLiteral(".gif.h264_"))
                                    ? QStringLiteral("gif")
                                    : QStringLiteral("mp4");
                        QString replacementName;
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            const QString candidate = h264PrinterName(
                                generatedPrinterMediaName(originalSuffix));
                            if (candidate != previousRemoteName &&
                                candidate != record.originalRemoteName &&
                                !files.contains(candidate)) {
                                replacementName = candidate;
                                break;
                            }
                        }
                        if (replacementName.isEmpty()) {
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Could not allocate a unique media name for the recovered transfer"),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                        record.remoteName = replacementName;
                        record.info.resultName = replacementName;
                        QString cacheError;
                        if (!writeRetryCache(
                                operationId,
                                record.info.terminalOutcome.isEmpty()
                                    ? QStringLiteral("PartialOrUnknown")
                                    : record.info.terminalOutcome,
                                &cacheError)) {
                            record.remoteName = previousRemoteName;
                            record.info.resultName =
                                previousRemoteName;
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Cannot persist the new retry target before upload: %1")
                                    .arg(cacheError),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                        record.info.confirmedBytes = 0;
                        record.info.lastConfirmedChunkIndex = -1;
                        record.info.terminalOutcome.clear();
                        record.info.state =
                            QStringLiteral("Preflight");
                        record.info.stage =
                            QStringLiteral("EnsuringSession");
                        record.info.message = tr(
                            "Retry preflight completed; the preserved media will be transferred under a new device filename");
                        publishOperation(operationId);
                        emit requestPrinterUploadPrepared(
                            currentPrinterPath(),
                            record.preparedPath,
                            record.remoteName,
                            record.preparedSha256,
                            operationId,
                            printerGeneration_);
                        return;
                    }
                    if (isSha256Hex(record.sourceContentSha256) &&
                        !record.conversionProfile.isEmpty()) {
                        const QString reusableName =
                            findReusableMediaOrigin(
                                record.sourceContentSha256,
                                record.conversionProfile, mediaFiles);
                        if (!reusableName.isEmpty() &&
                            reusableName != record.remoteName) {
                            record.remoteName = reusableName;
                            record.mediaFile = reusableName;
                            record.info.resultName = reusableName;
                            if (!clearRetryCache(true)) {
                                finishOperation(
                                    operationId,
                                    QStringLiteral("RetryAvailable"),
                                    QStringLiteral(
                                        "RetryCacheCleanupFailed"),
                                    QStringLiteral("PreparedMedia"),
                                    tr("An existing copy was found, but the retry manifest could not be removed"));
                                return;
                            }
                            removePreparedFileForOperation(operationId);
                            if (record.info.applyAfterUpload) {
                                record.info.state =
                                    QStringLiteral("Applying");
                                record.info.stage =
                                    QStringLiteral("ReusingExisting");
                                record.info.message = tr(
                                    "A confirmed copy already exists; applying it without retransmission...");
                                publishOperation(operationId);
                                emit requestPrinterApplyMedia(
                                    currentPrinterPath(), record.mediaFile,
                                    record.applyRequest,
                                    record.updateMetrics, operationId,
                                    printerGeneration_);
                                return;
                            }
                            finishOperation(
                                operationId,
                                QStringLiteral("Succeeded"), QString(),
                                QString(),
                                tr("A confirmed copy already exists; media data was not retransmitted"));
                            return;
                        }
                    }
                    if (exactPreparedFileCanBeTrusted) {
                        if (isSha256Hex(record.sourceContentSha256) &&
                            !record.conversionProfile.isEmpty()) {
                            TryxRuntimeMediaEntry verifiedOriginEntry;
                            verifiedOriginEntry.name = exactMedia->name;
                            verifiedOriginEntry.size = exactMedia->size;
                            verifiedOriginEntry.source = 1U;
                            verifiedOriginEntry.readOnly =
                                exactMedia->readOnly;
                            QString originError;
                            if (!persistMediaOriginForOperation(
                                    operationId, verifiedOriginEntry,
                                    &originError)) {
                                record.info.confirmedBytes = 0;
                                record.info.lastConfirmedChunkIndex = -1;
                                handlePreparedUploadFailure(
                                    operationId,
                                    tr("The previous upload is present, but its content identity could not be persisted: %1")
                                        .arg(originError),
                                    PrinterProtocol::MutationOutcome::NotStarted);
                                return;
                            }
                            updateMediaCatalog(mediaFiles);
                        }
                        if (!clearRetryCache(true)) {
                            finishOperation(
                                operationId,
                                QStringLiteral("RetryAvailable"),
                                QStringLiteral("RetryCacheCleanupFailed"),
                                QStringLiteral("PreparedMedia"),
                                tr("The media and preview were verified, but the retry manifest could not be removed"));
                            return;
                        }
                        removePreparedFileForOperation(operationId);
                        emit mediaUploaded(record.remoteName);
                        if (record.info.applyAfterUpload) {
                            record.mediaFile = record.remoteName;
                            record.info.state = QStringLiteral("Applying");
                            record.info.stage = QStringLiteral("Applying");
                            record.info.message = tr(
                                "The previous upload is present in FileList; applying it without retransmission...");
                            publishOperation(operationId);
                            emit requestPrinterApplyMedia(
                                currentPrinterPath(), record.mediaFile,
                                record.applyRequest, record.updateMetrics,
                                operationId, printerGeneration_);
                            return;
                        }
                        finishOperation(
                            operationId, QStringLiteral("Succeeded"),
                            QString(), QString(),
                            tr("The previous upload was verified in FileList; media data was not retransmitted"));
                        return;
                    }
                    if (files.contains(record.remoteName)) {
                        const QString previousRemoteName = record.remoteName;
                        const QString originalSuffix =
                            record.remoteName.contains(QStringLiteral(".png.h264_"))
                                ? QStringLiteral("png")
                                : record.remoteName.contains(QStringLiteral(".gif.h264_"))
                                    ? QStringLiteral("gif")
                                    : QStringLiteral("mp4");
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            const QString candidate = h264PrinterName(
                                generatedPrinterMediaName(originalSuffix));
                            if (!files.contains(candidate)) {
                                record.remoteName = candidate;
                                break;
                            }
                        }
                        if (files.contains(record.remoteName)) {
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Could not allocate a unique media name for retry"),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                        record.info.resultName = record.remoteName;
                        QString cacheError;
                        if (!writeRetryCache(
                                operationId,
                                record.info.terminalOutcome.isEmpty()
                                    ? QStringLiteral("NotStarted")
                                    : record.info.terminalOutcome,
                                &cacheError)) {
                            record.remoteName = previousRemoteName;
                            record.info.resultName = previousRemoteName;
                            handlePreparedUploadFailure(
                                operationId,
                                tr("Cannot persist the new retry target before upload: %1")
                                    .arg(cacheError),
                                PrinterProtocol::MutationOutcome::NotStarted);
                            return;
                        }
                    }
                    record.info.confirmedBytes = 0;
                    record.info.lastConfirmedChunkIndex = -1;
                    record.info.terminalOutcome.clear();
                    record.info.state = QStringLiteral("Preflight");
                    record.info.stage = QStringLiteral("EnsuringSession");
                    record.info.message = tr("Retry preflight completed");
                    publishOperation(operationId);
                    emit requestPrinterUploadPrepared(
                        currentPrinterPath(), record.preparedPath,
                        record.remoteName, record.preparedSha256,
                        operationId, printerGeneration_);
                    return;
                }
                if (record.info.stage != QStringLiteral("RefreshingMedia")) {
                    return;
                }
                if (!exactPreparedFilePresent) {
                    if (finalizationReconciliation) {
                        record.uploadFinalizationReconciliationPending =
                            false;
                        record.requiresDeviceRecovery = true;
                        record.retryMustUseNewRemoteName = true;
                        requirePrinterRecovery(tr(
                            "The final upload status was lost and FileList did not confirm the exact file. Power-cycle PASE before a manual retry."));
                    }
                    handlePreparedUploadFailure(
                        operationId,
                        files.contains(record.remoteName)
                            ? tr("The device acknowledged upload completion, but %1 does not match the prepared file size or source in FileList")
                                  .arg(record.remoteName)
                            : tr("The device acknowledged upload completion, but %1 is absent from FileList")
                                  .arg(record.remoteName),
                        PrinterProtocol::MutationOutcome::PartialOrUnknown);
                    return;
                }

                TryxRuntimeMediaEntry verifiedOriginEntry;
                verifiedOriginEntry.name = exactMedia->name;
                verifiedOriginEntry.size = exactMedia->size;
                verifiedOriginEntry.source = 1U;
                verifiedOriginEntry.readOnly = exactMedia->readOnly;
                if (record.ensureExisting ||
                    isSha256Hex(record.sourceContentSha256)) {
                    QString originError;
                    if (!persistMediaOriginForOperation(
                            operationId, verifiedOriginEntry,
                            &originError)) {
                        record.info.confirmedBytes = 0;
                        record.info.lastConfirmedChunkIndex = -1;
                        handlePreparedUploadFailure(
                            operationId,
                            tr("The media is present on the device, but its content identity could not be persisted: %1")
                                .arg(originError),
                            PrinterProtocol::MutationOutcome::NotStarted);
                        return;
                    }
                    updateMediaCatalog(mediaFiles);
                }

                if (retryCacheOperationId_ == operationId &&
                    retryCachePreparedPath_ == record.preparedPath) {
                    if (!clearRetryCache(true)) {
                        finishOperation(
                            operationId,
                            QStringLiteral("RetryAvailable"),
                            QStringLiteral("RetryCacheCleanupFailed"),
                            QStringLiteral("PreparedMedia"),
                            tr("The upload was verified, but the retry manifest could not be removed"));
                        return;
                    }
                }
                removePreparedFileForOperation(operationId);
                emit mediaUploaded(record.remoteName);
                if (record.cancelRequested) {
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        record.info.applyAfterUpload
                            ? tr("Upload completed before cancellation; apply was skipped")
                            : tr("Upload completed before cancellation"));
                    return;
                }
                if (record.info.applyAfterUpload) {
                    record.mediaFile = record.remoteName;
                    record.info.state = QStringLiteral("Applying");
                    record.info.stage = QStringLiteral("Applying");
                    record.info.message = tr("Applying the verified media...");
                    publishOperation(operationId);
                    emit requestPrinterApplyMedia(
                        currentPrinterPath(), record.mediaFile,
                        record.applyRequest, record.updateMetrics,
                        operationId, printerGeneration_);
                    return;
                }
                finishOperation(operationId, QStringLiteral("Succeeded"),
                                QString(), QString(),
                                tr("Media uploaded and verified"));
            });
    connect(worker_, &DeviceWorker::printerMediaListFailed, this,
            [this, printerResultIsCurrent,
             printerOperationResultIsExpected](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (operationId.isEmpty()) {
                    if (printerResultIsCurrent(generation)) {
                        emit deviceError(message);
                    }
                    return;
                }
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (record.originLookupPending) {
                    record.originLookupPending = false;
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("FileListUnavailable"), QString(),
                        tr("The existing-media check failed; upload was not started: %1")
                            .arg(message));
                    return;
                }
                if (record.uploadFinalizationReconciliationPending) {
                    record.uploadFinalizationReconciliationPending = false;
                    record.requiresDeviceRecovery = true;
                    requirePrinterRecovery(tr(
                        "The final upload status was lost and FileList could not be read. Power-cycle PASE before a manual retry."));
                }
                handlePreparedUploadFailure(
                    operationId, message,
                    record.retryPreflight
                        ? PrinterProtocol::MutationOutcome::NotStarted
                        : PrinterProtocol::MutationOutcome::PartialOrUnknown);
            });
    connect(worker_, &DeviceWorker::printerDeleteFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId,
                const QStringList &requestedNames,
                const QStringList &deletedNames,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.deletedNames = deletedNames;
                if (!record.deviceChangePending &&
                    (success ||
                     outcome == PrinterProtocol::MutationOutcome::Rejected ||
                     outcome ==
                         PrinterProtocol::MutationOutcome::NotStarted)) {
                    QStringList names;
                    QSet<QString> seen;
                    for (const PrinterProtocol::MediaFile &media :
                         mediaFiles) {
                        if (!seen.contains(media.name)) {
                            seen.insert(media.name);
                            names.append(media.name);
                        }
                    }
                    updateMediaCatalog(mediaFiles);
                    emit mediaListUpdated(names);
                }
                if (success) {
                    QString clearError;
                    if (!clearDeleteIntent(&clearError)) {
                        pendingDeleteOperationId_ = operationId;
                        finishOperation(
                            operationId,
                            QStringLiteral("RetryAvailable"),
                            QStringLiteral("PersistenceFailed"),
                            QStringLiteral("DeleteReconcile"),
                            tr("Deletion is confirmed, but its intent journal could not be removed: %1")
                                .arg(clearError));
                        return;
                    }
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        requestedNames.size() == 1
                            ? tr("Media file deleted and verified through FileList")
                            : tr("%1 media files deleted and verified through FileList")
                                  .arg(requestedNames.size()));
                    return;
                }
                if (outcome ==
                    PrinterProtocol::MutationOutcome::PartialOrUnknown) {
                    const int currentIndex = qBound(
                        0, deletedNames.size(),
                        qMax(0, requestedNames.size() - 1));
                    const QString currentName = requestedNames.value(
                        currentIndex, record.info.resultName);
                    pendingDeleteOperationId_ = operationId;
                    pendingDeleteIntent_.insert(
                        QStringLiteral("operationId"), operationId);
                    pendingDeleteIntent_.insert(
                        QStringLiteral("currentIndex"), currentIndex);
                    pendingDeleteIntent_.insert(
                        QStringLiteral("currentName"), currentName);
                    pendingDeleteIntent_.insert(
                        QStringLiteral("mayHaveStarted"), true);
                    record.info.resultName = currentName;
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("DeleteReconcile"),
                        tr("Delete outcome is unknown. FileRemove will not be repeated; only FileList reconciliation is allowed: %1")
                            .arg(errorMessage));
                    return;
                }

                QString clearError;
                if (!clearDeleteIntent(&clearError)) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral("PersistenceFailed"),
                        QStringLiteral("DeleteReconcile"),
                        tr("Delete did not complete, but its intent journal could not be cleared: %1")
                            .arg(clearError));
                    return;
                }
                const QString terminalState =
                    outcome == PrinterProtocol::MutationOutcome::Cancelled
                        ? QStringLiteral("Cancelled")
                        : QStringLiteral("Failed");
                finishOperation(operationId, terminalState,
                                mutationOutcomeName(outcome), QString(),
                                errorMessage);
            });
    connect(worker_, &DeviceWorker::printerApplyFinished, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, const QString &mediaFile,
                bool success, bool metricsUpdated,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.resultName = mediaFile;
                if (success) {
                    if (record.deviceChangePending) {
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("DeviceChanged"),
                            QStringLiteral("ReconcileOnly"),
                            record.deviceChangeMessage.isEmpty()
                                ? tr("USB changed before the applied PASE configuration could be associated with its original device")
                                : record.deviceChangeMessage);
                        return;
                    }
                    if (metricsUpdated) {
                        PrinterProtocol::PaseOverlayConfig overlay =
                            record.updateMetrics ||
                                    record.applyRequest.replaceOverlay
                                ? paseOverlayFromApplyRequest(
                                      record.applyRequest)
                                : persistedPaseOverlayForDevice(
                                      printerDeviceSerial_);
                        if (record.applyRequest.display.orientationPresent) {
                            overlay.waterfallMode =
                                record.applyRequest.display.waterfallMode;
                        }
                        QString metricsPersistenceError;
                        if (!persistPaseMetricsConfiguration(
                                overlay, paseOverlayHasContent(overlay),
                                &metricsPersistenceError)) {
                            metricsState_.deviceSerial =
                                printerDeviceSerial_.trimmed();
                            metricsState_.enabled =
                                paseOverlayHasMetrics(overlay);
                            metricsState_.samplingActive =
                                paseOverlayHasMetrics(overlay);
                            metricsState_.metrics =
                                overlay.left.metrics;
                            metricsState_.alignment =
                                overlay.left.alignment;
                            metricsState_.textColor =
                                overlay.left.textColor;
                            metricsState_.diagnostic = tr(
                                "The PASE metrics layout was applied but could not be persisted: %1")
                                                           .arg(metricsPersistenceError);
                            publishMetricsState();
                            finishOperation(
                                operationId, QStringLiteral("Failed"),
                                QStringLiteral("PersistenceFailed"),
                                QStringLiteral("ReconcileOnly"),
                                metricsState_.diagnostic);
                            emit screenConfigChanged();
                            return;
                        }
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled =
                            paseOverlayHasMetrics(overlay);
                        metricsState_.samplingActive =
                            paseOverlayHasMetrics(overlay);
                        metricsState_.metrics =
                            overlay.left.metrics;
                        metricsState_.alignment =
                            overlay.left.alignment;
                        metricsState_.textColor =
                            overlay.left.textColor;
                        metricsState_.diagnostic.clear();
                        publishMetricsState();
                        if (displayState_.valid) {
                            PrinterProtocol::PaseDisplayState state;
                            state.backlightEnabled =
                                displayState_.backlightEnabled;
                            state.brightness =
                                displayState_.brightness;
                            state.standbyEnabled =
                                displayState_.standbyEnabled;
                            state.standbyMedia =
                                displayState_.standbyMedia;
                            state.mirrorMode =
                                displayState_.mirrorMode;
                            state.waterfallMode =
                                displayState_.waterfallMode;
                            state.screenMode =
                                displayState_.screenMode;
                            state.playMode =
                                displayState_.playMode;
                            state.media = displayState_.media;
                            updateDisplayState(state, overlay);
                        }
                    }
                    finishOperation(operationId, QStringLiteral("Succeeded"),
                                    QString(), QString(),
                                    record.cancelRequested
                                        ? tr("Media was applied before cancellation completed")
                                        : tr("Media applied successfully"));
                    emit screenConfigChanged();
                    return;
                }
                if (record.cancelRequested &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(operationId, QStringLiteral("Cancelled"),
                                    QStringLiteral("UserCancelled"),
                                    QString(),
                                    tr("Operation cancelled by the user"));
                    return;
                }
                if (record.deviceChangePending &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(operationId, QStringLiteral("Cancelled"),
                                    QStringLiteral("DeviceChanged"),
                                    QString(), record.deviceChangeMessage);
                    return;
                }
                const QString retryMode =
                    outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
                        ? QStringLiteral("ReconcileOnly")
                        : QString();
                const QString category = mutationOutcomeName(outcome);
                finishOperation(operationId, QStringLiteral("Failed"),
                                category, retryMode, errorMessage);
            });
    connect(worker_, &DeviceWorker::printerMetricsConfigured, this,
            [this, printerOperationResultIsExpected](
                const QString &operationId, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                if (!printerOperationResultIsExpected(operationId,
                                                      generation)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (success && record.deviceChangePending) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("DeviceChanged"),
                        QStringLiteral("ReconcileOnly"),
                        record.deviceChangeMessage.isEmpty()
                            ? tr("USB changed before the PASE metrics layout could be associated with its original device")
                            : record.deviceChangeMessage);
                    return;
                }
                if (success) {
                    const PrinterProtocol::PaseOverlayConfig overlay =
                        paseOverlayFromMetricsRequest(record.metricsRequest);
                    QString persistenceError;
                    if (!persistPaseMetricsConfiguration(
                            overlay, record.metricsRequest.enabled,
                            &persistenceError)) {
                        metricsState_.deviceSerial =
                            printerDeviceSerial_.trimmed();
                        metricsState_.enabled = record.metricsRequest.enabled;
                        metricsState_.samplingActive =
                            record.metricsRequest.enabled;
                        metricsState_.metrics =
                            overlay.left.metrics;
                        metricsState_.alignment =
                            overlay.left.alignment;
                        metricsState_.textColor =
                            overlay.left.textColor;
                        metricsState_.diagnostic = tr(
                            "The PASE metrics layout was applied but could not be persisted: %1")
                                                       .arg(persistenceError);
                        publishMetricsState();
                        finishOperation(
                            operationId, QStringLiteral("Failed"),
                            QStringLiteral("PersistenceFailed"),
                            QStringLiteral("ReconcileOnly"),
                            metricsState_.diagnostic);
                        return;
                    }
                    metricsState_.deviceSerial =
                        printerDeviceSerial_.trimmed();
                    metricsState_.enabled = record.metricsRequest.enabled;
                    metricsState_.samplingActive =
                        record.metricsRequest.enabled;
                    metricsState_.metrics =
                        overlay.left.metrics;
                    metricsState_.alignment =
                        overlay.left.alignment;
                    metricsState_.textColor =
                        overlay.left.textColor;
                    metricsState_.diagnostic.clear();
                    publishMetricsState();
                    if (displayState_.valid) {
                        PrinterProtocol::PaseDisplayState state;
                        state.backlightEnabled =
                            displayState_.backlightEnabled;
                        state.brightness =
                            displayState_.brightness;
                        state.standbyEnabled =
                            displayState_.standbyEnabled;
                        state.standbyMedia =
                            displayState_.standbyMedia;
                        state.mirrorMode =
                            displayState_.mirrorMode;
                        state.waterfallMode =
                            displayState_.waterfallMode;
                        state.screenMode =
                            displayState_.screenMode;
                        state.playMode =
                            displayState_.playMode;
                        state.media = displayState_.media;
                        updateDisplayState(state, overlay);
                    }
                    finishOperation(
                        operationId, QStringLiteral("Succeeded"),
                        QString(), QString(),
                        record.metricsRequest.enabled
                            ? tr("PASE metrics configured successfully")
                            : tr("PASE metrics disabled successfully"));
                    return;
                }
                if (record.cancelRequested &&
                    (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
                     outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
                    finishOperation(
                        operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("UserCancelled"), QString(),
                        tr("Operation cancelled by the user"));
                    return;
                }
                const QString retryMode =
                    outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
                        ? QStringLiteral("ReconcileOnly")
                        : QString();
                finishOperation(operationId, QStringLiteral("Failed"),
                                mutationOutcomeName(outcome), retryMode,
                                errorMessage);
            });
    connect(worker_, &DeviceWorker::printerMetricsAvailabilityChanged, this,
            [this, printerResultIsCurrent](
                const QStringList &availableMetrics, quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    metricsState_.availableMetrics == availableMetrics) {
                    return;
                }
                metricsState_.availableMetrics = availableMetrics;
                publishMetricsState();
            });
    connect(worker_, &DeviceWorker::printerScreenConfigSet, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit screenConfigChanged();
                }
            });
    connect(worker_, &DeviceWorker::printerDeviceInfoReady, this,
            [this, printerResultIsCurrent](const PrinterProtocol::DeviceInfo &info,
                                           quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit printerDeviceInfoReady(info);
                }
            });
    connect(worker_, &DeviceWorker::printerDeviceInfoFailed, this,
            [this, printerResultIsCurrent](const QString &message, quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit printerDeviceInfoFailed(message);
                }
            });
    connect(worker_, &DeviceWorker::printerDisplayStateReady, this,
            [this, printerResultIsCurrent](
                const PrinterProtocol::PaseDisplayState &state,
                quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                updateDisplayState(
                    state,
                    persistedPaseOverlayForDevice(
                        printerDeviceSerial_));
            });
    connect(worker_, &DeviceWorker::printerDisplayStateFailed, this,
            [this, printerResultIsCurrent](
                const QString &message, quint64 generation) {
                if (!printerResultIsCurrent(generation)) {
                    return;
                }
                displayState_.deviceSerial =
                    printerDeviceSerial_.trimmed();
                displayState_.valid = false;
                displayState_.diagnostic =
                    tr("Failed to read PASE display state: %1")
                        .arg(message);
                publishDisplayState();
            });
#ifdef TRYX_PROTOCOL_TESTING
    connect(worker_, &DeviceWorker::printerDeviceInfoFailed, this,
            &DeviceManager::printerWorkerDeviceInfoFailedForTesting);
#endif

    connect(this, &DeviceManager::requestConnect,
            worker_, &DeviceWorker::connectDevice);
    connect(this, &DeviceManager::requestDisconnect,
            worker_, &DeviceWorker::disconnectDevice);
    connect(this, &DeviceManager::requestBrightness,
            worker_, &DeviceWorker::setBrightness);
    connect(this, &DeviceManager::requestScreenConfig,
            worker_, &DeviceWorker::setScreenConfig);
    connect(this, &DeviceManager::requestDeleteMedia,
            worker_, &DeviceWorker::deleteMedia);
    connect(this, &DeviceManager::requestUploadMedia,
            worker_, &DeviceWorker::uploadMedia);
    connect(this, &DeviceManager::requestRefreshMedia,
            worker_, &DeviceWorker::refreshMediaList);
    connect(this, &DeviceManager::requestKeepalive,
            worker_, &DeviceWorker::sendKeepalive);
    connect(this, &DeviceManager::requestRotation,
            worker_, &DeviceWorker::setRotation);
    connect(this, &DeviceManager::requestReboot,
            worker_, &DeviceWorker::rebootDevice);
    connect(this, &DeviceManager::requestSysinfo,
            worker_, &DeviceWorker::sendSysinfo);
    connect(this, &DeviceManager::requestConfigurePrinter,
            worker_, &DeviceWorker::configurePrinterDevice);
    connect(this, &DeviceManager::requestRestorePrinterOverlay,
            worker_, &DeviceWorker::restorePrinterOverlay);
    connect(this, &DeviceManager::requestBeginPrinterForegroundOperation,
            worker_, &DeviceWorker::beginPrinterForegroundOperation);
    connect(this, &DeviceManager::requestEndPrinterForegroundOperation,
            worker_, &DeviceWorker::endPrinterForegroundOperation);
    connect(this, &DeviceManager::requestClearPrinter,
            worker_, &DeviceWorker::clearPrinterDevice);
    connect(this, &DeviceManager::requestPrinterDeviceInfo,
            worker_, &DeviceWorker::readPrinterDeviceInfo);
    connect(this, &DeviceManager::requestPrinterDisplayState,
            worker_, &DeviceWorker::readPrinterDisplayState);
    connect(this, &DeviceManager::requestAnalyzePrinterSource,
            printerMediaPreparer_, &PrinterMediaPreparer::analyzeSource);
    connect(this, &DeviceManager::requestPrinterUploadPrepared,
            worker_, &DeviceWorker::uploadPreparedPrinterMedia);
    connect(this, &DeviceManager::requestPrinterRefreshMedia,
            worker_, &DeviceWorker::refreshPrinterMediaList);
    connect(this, &DeviceManager::requestPrinterDeleteMedia,
            worker_, &DeviceWorker::deletePrinterMedia);
    connect(this, &DeviceManager::requestPrinterApplyMedia,
            worker_, &DeviceWorker::applyPrinterMedia);
    connect(this, &DeviceManager::requestPrinterConfigureMetrics,
            worker_, &DeviceWorker::configurePrinterMetrics);
    connect(this, &DeviceManager::requestPrinterSysinfo,
            worker_, &DeviceWorker::sendPrinterSysinfo);
    if (automaticPrinterSessionStart_) {
        connect(this, &DeviceManager::requestStartPrinterSession,
                worker_, &DeviceWorker::startPrinterDisplaySession);
    }
    connect(worker_, &DeviceWorker::sysinfoSent, this,
            [this, legacyResultIsCurrent]() {
                if (legacyResultIsCurrent()) {
                    emit sysinfoSent();
                }
            });
    connect(worker_, &DeviceWorker::printerSysinfoSent, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit sysinfoSent();
                }
            });
    connect(worker_, &DeviceWorker::printerSysinfoFailed, this,
            [this, printerResultIsCurrent](const QString &message,
                                           quint64 generation) {
                if (printerResultIsCurrent(generation)) {
                    emit deviceError(
                        tr("Failed to update PASE metrics: %1").arg(message));
                }
            });
    connect(worker_, &DeviceWorker::printerTransportReady, this,
            [this, printerResultIsCurrent](quint64 generation) {
                if (printerResultIsCurrent(generation) &&
                    printerDisplaySessionActive_ &&
                    isPrinterClassDevicePresent() &&
                    activeOperationId_.isEmpty()) {
                    emit printerTransportReady();
                }
            });

    connect(this, &DeviceManager::requestPreparePrinterMedia,
            printerMediaPreparer_, &PrinterMediaPreparer::prepare);
    connect(
        this, &DeviceManager::requestCancelPrinterPreparation,
        this,
        [this](quint64 generation) {
            printerMediaPreparer_->requestGenerationCancellation(generation);
        },
        Qt::DirectConnection);
    connect(this, &DeviceManager::requestCancelPrinterPreparation,
            printerMediaPreparer_, &PrinterMediaPreparer::cancelStale);
    connect(
        this, &DeviceManager::requestCancelPrinterPreparationOperation,
        this,
        [this](const QString &operationId) {
            printerMediaPreparer_->requestOperationCancellation(operationId);
        },
        Qt::DirectConnection);
    connect(this, &DeviceManager::requestCancelPrinterPreparationOperation,
            printerMediaPreparer_, &PrinterMediaPreparer::cancelOperation);
    connect(this, &DeviceManager::requestReleasePrinterPreparation,
            printerMediaPreparer_, &PrinterMediaPreparer::releasePreparedFile);
    connect(this, &DeviceManager::requestValidatePrinterRetryCache,
            printerMediaPreparer_, &PrinterMediaPreparer::validateRetryCache);
    connect(printerMediaPreparer_,
            &PrinterMediaPreparer::retryCacheValidated,
            this, &DeviceManager::handleRetryCacheValidation);
    connect(worker_, &DeviceWorker::printerPreparedFileConsumed, this,
            [this](const QString &uploadPath) {
                emit requestReleasePrinterPreparation(uploadPath);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::sourceAnalyzed,
            this,
            [this, printerResultIsCurrent](
                const QString &operationId, const QString &localPath,
                const QString &contentSha256, qint64 sourceSize,
                const QString &conversionProfile, quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                if (!record.ensureExisting || record.cancelRequested ||
                    !isSha256Hex(contentSha256) || sourceSize <= 0 ||
                    conversionProfile.isEmpty() ||
                    QFileInfo(localPath).absoluteFilePath() !=
                        record.sourcePath) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceAnalysisFailed"), QString(),
                        tr("Source media identity could not be associated with the active operation"));
                    return;
                }
                const QString completedFingerprint =
                    sourceFingerprint(localPath);
                if (completedFingerprint.isEmpty() ||
                    completedFingerprint != record.sourceFingerprint) {
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceChanged"), QString(),
                        tr("Source media changed while its identity was being calculated"));
                    return;
                }
                record.sourceContentSha256 = contentSha256;
                record.sourceSize = sourceSize;
                record.conversionProfile = conversionProfile;
                record.originLookupPending = true;
                record.info.state = QStringLiteral("Refreshing");
                record.info.stage = QStringLiteral("RefreshingMedia");
                record.info.message = tr(
                    "Checking whether this media is already on the device...");
                publishOperation(operationId);
                emit requestBeginPrinterForegroundOperation(operationId,
                                                             generation);
                emit requestPrinterRefreshMedia(currentPrinterPath(),
                                                operationId, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::progress, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                OperationRecord &record = operations_[operationId];
                record.info.state = QStringLiteral("Converting");
                record.info.stage = QStringLiteral("Converting");
                record.info.message = message;
                publishOperation(operationId);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::failed, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &message,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    return;
                }
                const QString category =
                    operations_[operationId].info.stage ==
                            QStringLiteral("HashingSource")
                        ? QStringLiteral("SourceAnalysisFailed")
                        : QStringLiteral("ConversionFailed");
                finishOperation(operationId, QStringLiteral("Failed"),
                                category,
                                QString(), message);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::prepared, this,
            [this, printerResultIsCurrent](const QString &operationId,
                                           const QString &devicePath,
                                           const QString &sourcePath,
                                           const QString &uploadPath,
                                           const QString &remoteName,
                                           const QString &preparedSha256,
                                           const QString &stagedThumbnailPath,
                                           const QString &stagedThumbnailSha256,
                                           quint64 generation) {
                if (!printerResultIsCurrent(generation) ||
                    currentPrinterPath() != devicePath ||
                    activeOperationId_ != operationId ||
                    !operations_.contains(operationId)) {
                    QFile::remove(uploadPath);
                    emit requestReleasePrinterPreparation(uploadPath);
                    if (!stagedThumbnailPath.isEmpty()) {
                        QFile::remove(stagedThumbnailPath);
                        emit requestReleasePrinterPreparation(
                            stagedThumbnailPath);
                    }
                    return;
                }
                OperationRecord &record = operations_[operationId];
                const QString completedFingerprint =
                    sourceFingerprint(sourcePath);
                if (completedFingerprint.isEmpty() ||
                    completedFingerprint != record.sourceFingerprint) {
                    QFile::remove(uploadPath);
                    emit requestReleasePrinterPreparation(uploadPath);
                    if (!stagedThumbnailPath.isEmpty()) {
                        QFile::remove(stagedThumbnailPath);
                        emit requestReleasePrinterPreparation(
                            stagedThumbnailPath);
                    }
                    finishOperation(
                        operationId, QStringLiteral("Failed"),
                        QStringLiteral("SourceChanged"), QString(),
                        tr("Source media changed while it was being converted"));
                    return;
                }
                record.sourcePath = sourcePath;
                record.preparedPath = uploadPath;
                record.preparedSha256 = preparedSha256;
                record.stagedThumbnailPath = stagedThumbnailPath;
                record.stagedThumbnailSha256 = stagedThumbnailSha256;
                record.remoteName = remoteName;
                record.originalRemoteName = remoteName;
                record.info.resultName = remoteName;
                record.info.total = QFileInfo(uploadPath).size();
                record.info.state = QStringLiteral("Preflight");
                record.info.stage = QStringLiteral("EnsuringSession");
                record.info.message = tr("Prepared media is ready for upload");
                publishOperation(operationId);
                emit requestBeginPrinterForegroundOperation(operationId,
                                                             generation);
                emit requestPrinterUploadPrepared(devicePath, uploadPath,
                                                  remoteName, preparedSha256,
                                                  operationId,
                                                  generation);
            });

    connect(keepaliveTimer_, &QTimer::timeout, this, [this]() {
        if (!printerClassConnected_) {
            emit requestKeepalive();
        }
    });

    connect(printerMonitor_, &PrinterDeviceMonitor::snapshotChanged,
            this, &DeviceManager::handlePrinterSnapshot);
    connect(printerMonitor_, &PrinterDeviceMonitor::currentEndpointRemoved,
            this, [this]() {
                ++printerDisconnectCount_;
                const QString sysfsPath =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().sysfsPath
                        : QString();
                const QString serial =
                    printerSnapshot_.devices.size() == 1
                        ? printerSnapshot_.devices.first().serial.trimmed()
                        : printerDeviceSerial_.trimmed();
                logPrinterLifecycleEvent(
                    QStringLiteral("endpoint_removed"),
                    printerGeneration_,
                    {
                        {QStringLiteral("sysfs_path"), sysfsPath},
                        {QStringLiteral("serial"), serial},
                        {QStringLiteral("disconnect_count"),
                         QString::number(printerDisconnectCount_)},
                        {QStringLiteral("lease_mode"),
                         printerOverlayLeaseModeName(
                             printerOverlayLeaseMode_)}
                    });
                if (printerRecoveryRequired_ &&
                    isPrinterClassDevicePresent()) {
                    printerRecoveryRemovalObserved_ = true;
                }
                if (printerDisplaySessionLost_ &&
                    isPrinterClassDevicePresent()) {
                    printerSessionLossRemovalObserved_ = true;
                }
            });
    connect(printerMonitor_, &PrinterDeviceMonitor::monitorError,
            this, &DeviceManager::deviceError);

    printerPreparationThread_.start();
    workerThread_.start();
    if (startPrinterMonitor) {
        // The metadata scan is bounded and does not hash media. Reserve the
        // scheduler before the D-Bus service can accept foreground work; the
        // potentially large SHA-256 validation remains queued on the
        // preparation thread.
        loadMediaCatalogIndex();
        loadPaseMetricsConfig();
        loadRetryCache();
        loadDeleteIntent();
        printerMonitor_->start();
    }
}

#ifdef TRYX_PROTOCOL_TESTING
DeviceManager *DeviceManager::createForTesting(
    const QString &sysfsRoot, const QString &devRoot, QObject *parent) {
    auto *monitor = new PrinterDeviceMonitor;
    monitor->setDiscoveryRootsForTesting(sysfsRoot, devRoot);
    auto *manager = new DeviceManager(monitor, false, parent);
    manager->retryCacheDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("retry-cache"));
    manager->mediaCatalogDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("media-catalog"));
    manager->paseMetricsConfigDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("pase-config"));
    manager->loadMediaCatalogIndex();
    return manager;
}

void DeviceManager::setAutoConnectModeForTesting(bool enabled) {
    autoConnectMode_ = enabled;
}

void DeviceManager::rescanPrinterForTesting() {
    printerMonitor_->rescanForTesting(false);
}

void DeviceManager::injectPrinterUdevEventForTesting(
    const QByteArray &subsystem, const QString &syspath,
    const QString &sysname) {
    printerMonitor_->injectUdevEventForTesting(subsystem, syspath, sysname);
}

quint64 DeviceManager::printerGenerationForTesting() const {
    return printerGeneration_;
}

bool DeviceManager::printerDisplaySessionActiveForTesting() const {
    return printerDisplaySessionActive_;
}

bool DeviceManager::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    const bool adopted = QMetaObject::invokeMethod(
        worker_,
        [this, fd, devicePath]() {
            worker_->adoptPrinterFileDescriptorForTesting(fd, devicePath);
        },
        Qt::BlockingQueuedConnection);
    if (adopted && printerClassConnected_ &&
        devicePath == currentPrinterPath()) {
        emit requestStartPrinterSession(devicePath, printerGeneration_);
        if (!automaticPrinterSessionStart_) {
            QMetaObject::invokeMethod(
                worker_,
                [this, devicePath, generation = printerGeneration_]() {
                    worker_->startPrinterDisplaySession(devicePath, generation);
                },
                Qt::QueuedConnection);
        }
    }
    return adopted;
}

void DeviceManager::emitPrinterDeviceInfoFailureForTesting(
    const QString &message, quint64 generation) {
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, message, generation]() {
            emit worker->printerDeviceInfoFailed(message, generation);
        },
        Qt::QueuedConnection);
}

void DeviceManager::emitPrinterSessionLostForTesting(quint64 generation) {
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, generation]() {
            emit worker->printerSessionLost(generation);
        },
        Qt::QueuedConnection);
}
#endif

void DeviceManager::preserveActivePreparedMediaForShutdown() {
    if (activeOperationId_.isEmpty()) {
        return;
    }
    auto found = operations_.find(activeOperationId_);
    if (found == operations_.end() || found->cancelRequested ||
        found->preparedPath.isEmpty() ||
        !QFileInfo::exists(found->preparedPath) ||
        !isSha256Hex(found->preparedSha256)) {
        return;
    }

    const QString operationId = activeOperationId_;
    const QString preparedPath = found->preparedPath;
    const QString stagedThumbnailPath = found->stagedThumbnailPath;
    // Once prepared media has been dispatched to the USB worker, queued
    // progress can lag behind the actual write. Only validation and the
    // read-only retry FileList preflight are provably mutation-free here.
    const bool finalizationOnlyReconciliation =
        found->uploadFinalizationReconciliationPending;
    const bool mutationMayHaveStarted =
        !finalizationOnlyReconciliation &&
        !found->retryValidationPending && !found->retryPreflight;
    if (mutationMayHaveStarted) {
        found->requiresDeviceRecovery = true;
        found->retryMustUseNewRemoteName = true;
    }
    const QString outcome = finalizationOnlyReconciliation
        ? QStringLiteral("FinalizationUnknown")
        : mutationMayHaveStarted
            ? QStringLiteral("PartialOrUnknown")
            : QStringLiteral("NotStarted");
    QString cacheError;
    if (!writeRetryCache(operationId, outcome, &cacheError)) {
        qWarning().noquote()
            << tr("Cannot preserve prepared media while stopping the runtime: %1")
                   .arg(cacheError);
        return;
    }

    if (printerPreparationThread_.isRunning()) {
        QMetaObject::invokeMethod(
            printerMediaPreparer_,
            [preparer = printerMediaPreparer_, preparedPath,
             stagedThumbnailPath]() {
                preparer->releasePreparedFile(preparedPath);
                if (!stagedThumbnailPath.isEmpty()) {
                    preparer->releasePreparedFile(stagedThumbnailPath);
                }
            },
            Qt::BlockingQueuedConnection);
    }
    qInfo().noquote()
        << tr("Prepared media was preserved for a manual retry after runtime restart");
}

DeviceManager::~DeviceManager() {
    if (remoteMode_) {
        return;
    }
    if (!pendingRetryValidationId_.isEmpty()) {
        printerMediaPreparer_->cancelRetryValidation(
            pendingRetryValidationId_);
    }
    if (!activeOperationId_.isEmpty()) {
        const auto active = operations_.constFind(activeOperationId_);
        if (active != operations_.constEnd() &&
            active->retryValidationPending) {
            printerMediaPreparer_->cancelRetryValidation(
                activeOperationId_);
        }
    }
    preserveActivePreparedMediaForShutdown();
    disconnect(printerMonitor_, nullptr, this, nullptr);
    stopKeepalive();
    cancelForegroundForGenerationChange(
        tr("TRYX runtime is stopping"));
    ++printerGeneration_;
    emit requestCancelPrinterPreparation(printerGeneration_);
    worker_->updatePrinterGenerationGate(printerGeneration_, false);
    emit requestClearPrinter(printerGeneration_);
    emit requestDisconnect();
    if (printerPreparationThread_.isRunning()) {
        QMetaObject::invokeMethod(printerMediaPreparer_, "shutdown",
                                  Qt::BlockingQueuedConnection);
        printerPreparationThread_.quit();
        printerPreparationThread_.wait();
    }
    workerThread_.quit();
    workerThread_.wait();
}

void DeviceManager::setPrinterDisplaySessionActive(bool active) {
    if (printerDisplaySessionActive_ == active) {
        return;
    }
    printerDisplaySessionActive_ = active;
    emit printerDisplaySessionChanged(active);
}

void DeviceManager::handlePrinterSnapshot(
    const PrinterProtocol::DiscoverySnapshot &snapshot) {
    const bool oldPresence = isPrinterClassDevicePresent();
    const bool wasPrinterConnected = printerClassConnected_;
    const QString oldPath = printerDevicePath_;
    const QString oldSerial = printerDeviceSerial_;

    if (printerRecoveryRequired_ &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        printerRecoveryRemovalObserved_ = true;
    }
    if (printerDisplaySessionLost_ &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        printerSessionLossRemovalObserved_ = true;
    }

    if (wasPrinterConnected && printerDisplaySessionActive_ &&
        !oldSerial.isEmpty()) {
        printerSessionResumePending_ = true;
        printerSessionResumeSerial_ = oldSerial;
    }
    setPrinterDisplaySessionActive(false);

    cancelForegroundForGenerationChange(
        tr("Printer-class operation stopped because the USB connection changed"));
    printerSnapshot_ = snapshot;
    ++printerGeneration_;
    printerGenerationElapsedTimer_.start();
    emit requestCancelPrinterPreparation(printerGeneration_);
    if (wasPrinterConnected) {
        emit printerOperationsCancelled();
    }
    const bool ready = snapshot.state == PrinterProtocol::DiscoveryState::Ready &&
                       snapshot.devices.size() == 1;
    const bool endpointSelected = ready &&
                                  (autoConnectMode_ || wasPrinterConnected);
    const QString snapshotSysfsPath = snapshot.devices.size() == 1
        ? snapshot.devices.first().sysfsPath
        : QString();
    const QString snapshotSerial = snapshot.devices.size() == 1
        ? snapshot.devices.first().serial.trimmed()
        : QString();
    logPrinterLifecycleEvent(
        QStringLiteral("physical_generation_changed"),
        printerGeneration_,
        {
            {QStringLiteral("discovery_state"),
             printerDiscoveryStateName(snapshot.state)},
            {QStringLiteral("sysfs_path"), snapshotSysfsPath},
            {QStringLiteral("serial"), snapshotSerial},
            {QStringLiteral("endpoint_selected"),
             endpointSelected ? QStringLiteral("true")
                              : QStringLiteral("false")},
            {QStringLiteral("disconnect_count"),
             QString::number(printerDisconnectCount_)},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(
                 printerOverlayLeaseMode_)}
        });
    if (ready) {
        logPrinterLifecycleEvent(
            QStringLiteral("endpoint_discovered"),
            printerGeneration_,
            {
                {QStringLiteral("sysfs_path"),
                 snapshotSysfsPath},
                {QStringLiteral("serial"), snapshotSerial},
                {QStringLiteral("endpoint_selected"),
                 endpointSelected ? QStringLiteral("true")
                                  : QStringLiteral("false")},
                {QStringLiteral("disconnect_count"),
                 QString::number(printerDisconnectCount_)}
            });
    }
    const bool retryCacheValidationComplete =
        pendingRetryValidationId_.isEmpty();
    const bool sessionLossAllowsSession =
        !printerDisplaySessionLost_ ||
        printerSessionLossRemovalObserved_;
    const bool recoveryAllowsSession =
        retryCacheValidationComplete &&
        sessionLossAllowsSession &&
        (!printerRecoveryRequired_ ||
         (endpointSelected &&
          completePrinterRecoveryAfterRemoval(
              snapshot.devices.first().serial)));
    const bool resumeSelectedSession =
        endpointSelected && printerSessionResumePending_ &&
        !snapshot.devices.first().serial.isEmpty() &&
        snapshot.devices.first().serial == printerSessionResumeSerial_;
    if (ready && printerSessionResumePending_ && !resumeSelectedSession) {
        printerSessionResumePending_ = false;
        printerSessionResumeSerial_.clear();
    }
    worker_->updatePrinterGenerationGate(
        printerGeneration_, endpointSelected && recoveryAllowsSession);

    if (endpointSelected) {
        const QString newPath = snapshot.devices.first().devicePath;
        const QString newSerial = snapshot.devices.first().serial;
        if (wasPrinterConnected &&
            (oldPath != newPath || oldSerial != newSerial)) {
            detachPrinterClassDevice(true);
        }
        emit requestConfigurePrinter(
            newPath, newSerial, printerGeneration_);
        const PrinterProtocol::PaseOverlayConfig restoredOverlay =
            persistedPaseOverlayForDevice(newSerial);
        metricsState_.deviceSerial = newSerial.trimmed();
        metricsState_.enabled = paseOverlayHasMetrics(restoredOverlay);
        metricsState_.samplingActive = false;
        metricsState_.metrics = restoredOverlay.left.metrics;
        metricsState_.alignment = restoredOverlay.left.alignment;
        metricsState_.textColor = restoredOverlay.left.textColor;
        metricsState_.availableMetrics.clear();
        metricsState_.diagnostic.clear();
        publishMetricsState();
        if (recoveryAllowsSession &&
            paseOverlayHasContent(restoredOverlay)) {
            emit requestRestorePrinterOverlay(
                restoredOverlay, printerGeneration_);
        }
        if (resumeSelectedSession) {
            emit uploadStatus(
                tr("Restoring the active PASE display session after USB re-enumeration..."));
        }
        if (recoveryAllowsSession) {
            printerDisplaySessionLost_ = false;
            printerSessionLossRemovalObserved_ = false;
            emit requestStartPrinterSession(newPath, printerGeneration_);
        } else if (!retryCacheValidationComplete &&
                   !printerDisplaySessionLost_) {
            printerDisplaySessionLost_ = false;
            emit uploadStatus(tr(
                "Stored retry media is still being validated; the PASE display session will start only after validation finishes"));
        } else {
            printerDisplaySessionLost_ = true;
            emit uploadStatus(printerMutationUnavailableStatusText());
        }
    } else {
        emit requestClearPrinter(printerGeneration_);
        if (wasPrinterConnected) {
            detachPrinterClassDevice(true);
        }
    }
    if (wasPrinterConnected) {
        emit mediaListUpdated({});
    }

    if (snapshot.blocksLegacyTransport() && connected_ &&
        !printerClassConnected_) {
        connected_ = false;
        emit mediaListUpdated({});
        emit requestDisconnect();
    }

    const bool newPresence = isPrinterClassDevicePresent();
    if (oldPresence != newPresence) {
        emit printerPresenceChanged(newPresence);
    }

    if (!autoConnectMode_) {
        return;
    }

    switch (snapshot.state) {
    case PrinterProtocol::DiscoveryState::Ready:
        if (!printerClassConnected_ && snapshot.devices.size() == 1) {
            attachPrinterClassDevice(snapshot.devices.first());
        }
        break;
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
    case PrinterProtocol::DiscoveryState::Enumerating391a1021:
        emit uploadStatus(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::PermissionDenied:
    case PrinterProtocol::DiscoveryState::Ambiguous:
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        emit deviceError(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::Absent:
        if (!connected_) {
            const auto legacyPort = panorama::Device::find_device();
            if (legacyPort) {
                emit requestConnect(QString::fromStdString(*legacyPort));
            } else {
                emit uploadStatus(
                    tr("Waiting for TRYX device. Reconnect USB or keep Auto connection selected."));
            }
        }
        break;
    }

}

void DeviceManager::connectDevice(const QString &port) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("ConnectDevice"), {port});
        return;
    }
    if (!port.isEmpty() && printerSnapshot_.blocksLegacyTransport()) {
        emit deviceError(
            tr("A TRYX printer-class or Rockchip gadget device is present; use Auto connection."));
        return;
    }
    setPrinterDisplaySessionActive(false);
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    autoConnectMode_ = port.isEmpty();

    if (port.isEmpty()) {
        if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready &&
            printerSnapshot_.devices.size() == 1) {
            cancelForegroundForGenerationChange(
                tr("Printer-class connection was restarted"));
            ++printerGeneration_;
            printerGenerationElapsedTimer_.start();
            emit requestCancelPrinterPreparation(printerGeneration_);
            const bool retryCacheValidationComplete =
                pendingRetryValidationId_.isEmpty();
            const bool sessionLossAllowsSession =
                !printerDisplaySessionLost_ ||
                printerSessionLossRemovalObserved_;
            const bool recoveryAllowsSession =
                retryCacheValidationComplete &&
                sessionLossAllowsSession &&
                (!printerRecoveryRequired_ ||
                 completePrinterRecoveryAfterRemoval(
                     printerSnapshot_.devices.first().serial));
            worker_->updatePrinterGenerationGate(
                printerGeneration_, recoveryAllowsSession);
            emit requestConfigurePrinter(
                printerSnapshot_.devices.first().devicePath,
                printerSnapshot_.devices.first().serial,
                printerGeneration_);
            const PrinterProtocol::PaseOverlayConfig restoredOverlay =
                persistedPaseOverlayForDevice(
                    printerSnapshot_.devices.first().serial);
            metricsState_.deviceSerial =
                printerSnapshot_.devices.first().serial.trimmed();
            metricsState_.enabled =
                paseOverlayHasMetrics(restoredOverlay);
            metricsState_.samplingActive = false;
            metricsState_.metrics = restoredOverlay.left.metrics;
            metricsState_.alignment =
                restoredOverlay.left.alignment;
            metricsState_.textColor =
                restoredOverlay.left.textColor;
            metricsState_.availableMetrics.clear();
            metricsState_.diagnostic.clear();
            publishMetricsState();
            if (recoveryAllowsSession &&
                paseOverlayHasContent(restoredOverlay)) {
                emit requestRestorePrinterOverlay(
                    restoredOverlay, printerGeneration_);
            }
            attachPrinterClassDevice(printerSnapshot_.devices.first());
            if (recoveryAllowsSession) {
                printerDisplaySessionLost_ = false;
                printerSessionLossRemovalObserved_ = false;
                emit requestStartPrinterSession(
                    printerSnapshot_.devices.first().devicePath,
                    printerGeneration_);
            } else if (!retryCacheValidationComplete &&
                       !printerDisplaySessionLost_) {
                printerDisplaySessionLost_ = false;
                emit uploadStatus(tr(
                    "Stored retry media is still being validated; the PASE display session will start only after validation finishes"));
            } else {
                printerDisplaySessionLost_ = true;
                emit uploadStatus(printerMutationUnavailableStatusText());
            }
            return;
        }
        if (printerSnapshot_.blocksLegacyTransport()) {
            connected_ = false;
            printerClassConnected_ = false;
            printerDevicePath_.clear();
            const QString status = printerSnapshot_.statusText();
            if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::PermissionDenied ||
                printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ambiguous ||
                printerSnapshot_.state ==
                    PrinterProtocol::DiscoveryState::MonitoringUnavailable) {
                emit deviceError(status);
            } else {
                emit uploadStatus(status);
            }
            return;
        }

        const auto legacyPort = panorama::Device::find_device();
        if (!legacyPort) {
            connected_ = false;
            printerClassConnected_ = false;
            printerDevicePath_.clear();
            emit uploadStatus(
                tr("Waiting for TRYX device. Reconnect USB or keep Auto connection selected."));
            return;
        }
        emit requestConnect(QString::fromStdString(*legacyPort));
        return;
    }

    detachPrinterClassDevice(false);
    emit requestConnect(port);
}

void DeviceManager::disconnectDevice() {
    if (remoteMode_) {
        remoteCall(QStringLiteral("DisconnectDevice"));
        return;
    }
    autoConnectMode_ = false;
    setPrinterDisplaySessionActive(false);
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    stopKeepalive();
    const bool notifyPrinterDisconnect = printerClassConnected_;
    detachPrinterClassDevice(false);
    cancelForegroundForGenerationChange(
        tr("Printer-class operation stopped because the device was disconnected"));
    ++printerGeneration_;
    emit requestCancelPrinterPreparation(printerGeneration_);
    if (notifyPrinterDisconnect) {
        emit printerOperationsCancelled();
    }
    worker_->updatePrinterGenerationGate(printerGeneration_, false);
    emit requestClearPrinter(printerGeneration_);
    emit requestDisconnect();
    connected_ = false;
    emit mediaListUpdated({});
    if (notifyPrinterDisconnect) {
        emit deviceDisconnected();
    }
}

void DeviceManager::requestDeviceInfo() {
    if (remoteMode_) {
        remoteCall(QStringLiteral("RequestDeviceInfo"));
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (!devicePath.isEmpty()) {
        if (!pendingRetryValidationId_.isEmpty() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_) {
            emit printerDeviceInfoFailed(
                printerMutationUnavailableStatusText());
            return;
        }
        emit requestPrinterDeviceInfo(devicePath, printerGeneration_);
        return;
    }
    if (printerSnapshot_.blocksLegacyTransport()) {
        emit printerDeviceInfoFailed(printerSnapshot_.statusText());
        return;
    }
    if (connected_) {
        emit uploadStatus(tr("Legacy device information is available from its connection handshake."));
        return;
    }
    emit printerDeviceInfoFailed(tr("TRYX device is not connected"));
}

bool DeviceManager::isPrinterClassDevicePresent() const {
    if (remoteMode_) {
        return remotePrinterClassDevicePresent_;
    }
    return printerClassConnected_ || printerSnapshot_.blocksLegacyTransport();
}

void DeviceManager::attachPrinterClassDevice(
    const PrinterProtocol::UsbPrinterDevice &device) {
    if (printerClassConnected_ && printerDevicePath_ == device.devicePath) {
        return;
    }
    stopKeepalive();
    connected_ = true;
    printerClassConnected_ = true;
    setPrinterDisplaySessionActive(false);
    printerDevicePath_ = device.devicePath;
    printerDeviceSerial_ = device.serial;
    clearMediaCatalogView();
    emit mediaListUpdated({});
    emit deviceConnected(tr("USB printer-class"),
                         device.serial.isEmpty() ? device.devicePath : device.serial,
                         QString(), QString());
}

void DeviceManager::detachPrinterClassDevice(bool notify) {
    const bool wasConnected = printerClassConnected_;
    printerClassConnected_ = false;
    printerDevicePath_.clear();
    printerDeviceSerial_.clear();
    if (wasConnected) {
        displayStateReadGeneration_ = 0;
        const quint64 metricsRevision = metricsState_.revision;
        metricsState_ = TryxRuntimeMetricsState{};
        metricsState_.revision = metricsRevision;
        publishMetricsState();
        const quint64 displayRevision = displayState_.revision;
        displayState_ = TryxRuntimeDisplayState{};
        displayState_.revision = displayRevision;
        publishDisplayState();
        connected_ = false;
        clearMediaCatalogView();
        emit mediaListUpdated({});
        if (notify) {
            emit deviceDisconnected();
        }
    }
}

QString DeviceManager::currentPrinterPath() const {
    if (!printerClassConnected_ ||
        printerSnapshot_.state != PrinterProtocol::DiscoveryState::Ready ||
        printerSnapshot_.devices.size() != 1) {
        return {};
    }
    return printerSnapshot_.devices.first().devicePath;
}

QString DeviceManager::printerUnavailableStatusText() const {
    if (printerSnapshot_.state == PrinterProtocol::DiscoveryState::Ready &&
        printerSnapshot_.devices.size() == 1 && !printerClassConnected_) {
        return tr(
            "TRYX endpoint is present, but the display session stopped. Reconnect USB or select Auto connection again.");
    }
    return printerSnapshot_.statusText();
}

QString DeviceManager::printerMutationUnavailableStatusText() const {
    if (!pendingRetryValidationId_.isEmpty()) {
        return tr(
            "Stored retry media is still being validated; wait for validation to finish before using the PASE display session.");
    }
    if (printerRecoveryRequired_) {
        return tr(
            "PASE must be power-cycled before another upload or display change. Disconnect its USB/power while the TRYX runtime is running, reconnect it, and wait for the display session to become active.");
    }
    if (printerDisplaySessionLost_) {
        return tr(
            "The PASE display session is lost. Reconnect the device and wait for a new display session before trying again.");
    }
    if (!printerDisplaySessionActive_) {
        return tr(
            "The PASE display session is not ready yet. Wait until the device finishes connecting before trying again.");
    }
    return printerUnavailableStatusText();
}

void DeviceManager::resumePrinterSessionAfterRetryCacheValidation() {
    if (remoteMode_ || !worker_ ||
        !pendingRetryValidationId_.isEmpty() ||
        printerRecoveryRequired_) {
        return;
    }
    if (printerDisplaySessionLost_ &&
        !printerSessionLossRemovalObserved_) {
        emit uploadStatus(printerMutationUnavailableStatusText());
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        return;
    }

    worker_->updatePrinterGenerationGate(printerGeneration_, true);
    const PrinterProtocol::PaseOverlayConfig restoredOverlay =
        persistedPaseOverlayForDevice(printerDeviceSerial_);
    if (paseOverlayHasContent(restoredOverlay)) {
        emit requestRestorePrinterOverlay(
            restoredOverlay, printerGeneration_);
    }
    printerDisplaySessionLost_ = false;
    printerSessionLossRemovalObserved_ = false;
    emit requestStartPrinterSession(devicePath, printerGeneration_);
}

void DeviceManager::requirePrinterRecovery(const QString &message) {
    const bool enteringRecovery = !printerRecoveryRequired_;
    printerRecoveryRequired_ = true;
    if (enteringRecovery) {
        printerRecoveryRemovalObserved_ = false;
    }
    printerDisplaySessionLost_ = true;
    printerSessionLossRemovalObserved_ = false;
    setPrinterDisplaySessionActive(false);
    printerSessionResumePending_ = false;
    printerSessionResumeSerial_.clear();
    if (!remoteMode_ && worker_) {
        if (enteringRecovery) {
            ++printerGeneration_;
            emit requestCancelPrinterPreparation(printerGeneration_);
        }
        worker_->updatePrinterGenerationGate(printerGeneration_, false);
        emit requestClearPrinter(printerGeneration_);
    }
    if (!message.isEmpty()) {
        emit uploadStatus(message);
    }
}

bool DeviceManager::completePrinterRecoveryAfterRemoval(
    const QString &currentDeviceIdentity) {
    if (!printerRecoveryRequired_) {
        return true;
    }
    if (!printerRecoveryRemovalObserved_) {
        return false;
    }

    if (!retryCacheOperationId_.isEmpty() &&
        operations_.contains(retryCacheOperationId_)) {
        OperationRecord &record = operations_[retryCacheOperationId_];
        const QString observedIdentity = currentDeviceIdentity.trimmed();
        const QString expectedIdentity =
            record.uploadDeviceIdentity.trimmed();
        if (expectedIdentity.isEmpty()) {
            record.info.message = tr(
                "The original PASE identity is unavailable. Prepared media cannot be retried automatically.");
            publishOperation(retryCacheOperationId_);
            emit deviceError(record.info.message);
            return false;
        }
        if (observedIdentity.isEmpty()) {
            record.info.message = tr(
                "PASE was reconnected, but its device identity is unavailable. Retry remains blocked.");
            publishOperation(retryCacheOperationId_);
            emit deviceError(record.info.message);
            return false;
        }
        if (!expectedIdentity.isEmpty() &&
            expectedIdentity != observedIdentity) {
            record.info.message = tr(
                "A different PASE was connected after the incomplete transfer. Reconnect the original device before Retry.");
            publishOperation(retryCacheOperationId_);
            emit deviceError(record.info.message);
            return false;
        }
        const bool previousRecoveryState = record.requiresDeviceRecovery;
        const QString previousMessage = record.info.message;
        const QString previousIdentity = record.uploadDeviceIdentity;
        record.requiresDeviceRecovery = false;
        record.uploadDeviceIdentity = observedIdentity;
        record.info.message = tr(
            "The same PASE was physically reconnected after the incomplete transfer. Prepared media can now be transferred again under a new device filename.");
        QString cacheError;
        if (!writeRetryCache(
                retryCacheOperationId_,
                record.info.terminalOutcome.isEmpty()
                    ? QStringLiteral("PartialOrUnknown")
                    : record.info.terminalOutcome,
                &cacheError)) {
            record.requiresDeviceRecovery = previousRecoveryState;
            record.info.message = previousMessage;
            record.uploadDeviceIdentity = previousIdentity;
            emit deviceError(
                tr("PASE reconnected, but the recovery state could not be saved: %1")
                    .arg(cacheError));
            return false;
        }
        publishOperation(retryCacheOperationId_);
    }

    printerRecoveryRequired_ = false;
    printerRecoveryRemovalObserved_ = false;
    printerDisplaySessionLost_ = false;
    printerSessionLossRemovalObserved_ = false;
    emit uploadStatus(
        tr("PASE power-cycle was observed; starting a clean display session"));
    return true;
}

QString DeviceManager::normalizedOperationId(const QString &requestedId) const {
    const QString trimmed = requestedId.trimmed();
    const QUuid parsed(trimmed);
    if (!trimmed.isEmpty() && !parsed.isNull()) {
        return parsed.toString(QUuid::WithoutBraces);
    }
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool DeviceManager::operationIsTerminal(const QString &state) const {
    return state == QStringLiteral("Succeeded") ||
           state == QStringLiteral("Failed") ||
           state == QStringLiteral("Cancelled") ||
           state == QStringLiteral("RetryAvailable");
}

TryxRuntimeOperationsSnapshot DeviceManager::operationSnapshot() const {
    TryxRuntimeOperationsSnapshot snapshot;
    snapshot.revision = remoteMode_ ? remoteOperationRevision_
                                    : operationRevision_;
    snapshot.activeOperationId = activeOperationId_;
    for (const QString &operationId : operationOrder_) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd()) {
            snapshot.operations.append(found->info);
        }
    }
    return snapshot;
}

TryxRuntimeOperationInfo DeviceManager::operationInfo(
    const QString &operationId) const {
    const auto found = operations_.constFind(operationId);
    return found == operations_.constEnd() ? TryxRuntimeOperationInfo{}
                                           : found->info;
}

TryxRuntimeOperationInfo DeviceManager::activeOperationInfo() const {
    return operationInfo(activeOperationId_);
}

QStringList DeviceManager::metricsCapabilities() const {
    return {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("CPU Usage"),
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Frequency"),
        QStringLiteral("GPU Usage"),
        QStringLiteral("GPU Power"),
        QStringLiteral("Memory Frequency"),
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time"),
    };
}

void DeviceManager::publishMetricsState() {
    if (remoteMode_) {
        emit metricsStateUpdated(metricsState_);
        return;
    }
    ++metricsState_.revision;
    emit metricsStateUpdated(metricsState_);
}

void DeviceManager::publishDisplayState() {
    if (remoteMode_) {
        emit displayStateUpdated(displayState_);
        return;
    }
    ++displayState_.revision;
    emit displayStateUpdated(displayState_);
}

void DeviceManager::updateDisplayState(
    const PrinterProtocol::PaseDisplayState &state,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    const int previousBrightness = displayState_.brightness;
    const bool hadValidState = displayState_.valid;
    displayState_.deviceSerial = printerDeviceSerial_.trimmed();
    displayState_.valid = true;
    displayState_.backlightEnabled = state.backlightEnabled;
    displayState_.brightness = state.brightness;
    displayState_.standbyEnabled = state.standbyEnabled;
    displayState_.standbyMedia = state.standbyMedia;
    displayState_.mirrorMode = state.mirrorMode;
    displayState_.waterfallMode = state.waterfallMode;
    displayState_.screenMode = state.screenMode;
    displayState_.playMode = state.playMode;
    displayState_.media = state.media;
    displayState_.sysinfoLabels = overlay.left.metrics;
    displayState_.settingsBadges = overlay.left.badges;
    displayState_.settingsPosition =
        overlay.left.verticalPlacement;
    displayState_.settingsColor =
        paseTextColorName(overlay.left.textColor);
    displayState_.settingsAlign = overlay.left.alignment;
    if (overlay.dualMode) {
        displayState_.sysinfoLabels2 = overlay.right.metrics;
        displayState_.settingsBadges2 = overlay.right.badges;
        displayState_.settingsPosition2 =
            overlay.right.verticalPlacement;
        displayState_.settingsColor2 =
            paseTextColorName(overlay.right.textColor);
        displayState_.settingsAlign2 =
            overlay.right.alignment;
    } else {
        displayState_.sysinfoLabels2.clear();
        displayState_.settingsBadges2.clear();
        displayState_.settingsPosition2.clear();
        displayState_.settingsColor2.clear();
        displayState_.settingsAlign2.clear();
    }
    displayState_.diagnostic.clear();
    publishDisplayState();
    if (!hadValidState ||
        previousBrightness != displayState_.brightness) {
        emit brightnessChanged(displayState_.brightness);
    }
}

TryxRuntimeMediaCatalogSnapshot DeviceManager::mediaCatalogSnapshot() const {
    return mediaCatalog_;
}

QString DeviceManager::mediaCatalogDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!mediaCatalogDirectoryOverride_.isEmpty()) {
        return mediaCatalogDirectoryOverride_;
    }
#endif
    return QDir(QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("media-catalog"));
}

QString DeviceManager::mediaThumbnailDirectory() const {
    return QDir(mediaCatalogDirectory())
        .filePath(QStringLiteral("thumbnails"));
}

QString DeviceManager::mediaCatalogIndexPath() const {
    return QDir(mediaCatalogDirectory()).filePath(QStringLiteral("index.json"));
}

QString DeviceManager::mediaThumbnailPath(
    const QString &thumbnailKey) const {
    if (!isSha256Hex(thumbnailKey)) {
        return {};
    }
    const QString path = QDir(mediaThumbnailDirectory())
                             .filePath(thumbnailKey + QStringLiteral(".jpg"));
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymLink()
        ? path
        : QString();
}

QString DeviceManager::mediaThumbnailKey(
    const QString &deviceIdentity,
    const TryxRuntimeMediaEntry &entry) const {
    if (deviceIdentity.trimmed().isEmpty() || entry.name.isEmpty()) {
        return {};
    }
    const QByteArray identity =
        deviceIdentity.toUtf8() + '\0' + entry.name.toUtf8() + '\0' +
        QByteArray::number(entry.size) + '\0' +
        QByteArray::number(entry.source);
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
            .toHex());
}

void DeviceManager::loadMediaCatalogIndex() {
    mediaCatalogIndex_ = QJsonObject{};
    mediaCatalogWriteEnabled_ = true;
    const QString catalogDirectory = mediaCatalogDirectory();
    const QString thumbnailDirectory = mediaThumbnailDirectory();
    if (!QDir().mkpath(catalogDirectory) ||
        !QDir().mkpath(thumbnailDirectory)) {
        qWarning() << "Cannot create media catalog directories";
        return;
    }

    const QFileInfo indexInfo(mediaCatalogIndexPath());
    if (!indexInfo.exists()) {
        sweepMediaThumbnailOrphans();
        return;
    }
    if (!indexInfo.isFile() || indexInfo.isSymLink() ||
        indexInfo.size() <= 0 ||
        indexInfo.size() > kMaxMediaCatalogIndexBytes) {
        qWarning() << "Ignoring unsafe media catalog index";
        return;
    }
    QFile file(indexInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read media catalog index" << file.errorString();
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        qWarning() << "Ignoring malformed media catalog index"
                   << parseError.errorString();
        return;
    }
    const QJsonObject root = document.object();
    const int storedVersion =
        root.value(QStringLiteral("version")).toInt(-1);
    if ((storedVersion != 1 &&
         storedVersion != kMediaCatalogFormatVersion) ||
        !root.value(QStringLiteral("entries")).isObject()) {
        qWarning() << "Ignoring unsupported media catalog index";
        return;
    }

    const QJsonObject storedEntries =
        root.value(QStringLiteral("entries")).toObject();
    for (auto it = storedEntries.constBegin(); it != storedEntries.constEnd();
         ++it) {
        if (!isSha256Hex(it.key()) || !it.value().isObject()) {
            continue;
        }
        QJsonObject record = it.value().toObject();
        const QString deviceIdentity =
            record.value(QStringLiteral("deviceIdentity")).toString().trimmed();
        const QString name =
            record.value(QStringLiteral("name")).toString();
        bool sizeOk = false;
        const quint64 size =
            record.value(QStringLiteral("size")).toString().toULongLong(
                &sizeOk);
        const int source = record.value(QStringLiteral("source")).toInt(0);
        if (deviceIdentity.isEmpty() ||
            !PrinterProtocol::isSafeUploadMediaName(name) || !sizeOk ||
            size == 0 || (source != 1 && source != 2) ||
            !record.value(QStringLiteral("readOnly")).isBool()) {
            continue;
        }
        TryxRuntimeMediaEntry identityEntry;
        identityEntry.name = name;
        identityEntry.size = size;
        identityEntry.source = static_cast<quint32>(source);
        identityEntry.readOnly =
            record.value(QStringLiteral("readOnly")).toBool();
        if (mediaThumbnailKey(deviceIdentity, identityEntry) != it.key()) {
            continue;
        }

        const QString expectedHash = storedVersion == 1
            ? record.value(QStringLiteral("sha256")).toString()
            : record.value(QStringLiteral("thumbnailSha256")).toString();
        const QString path = QDir(thumbnailDirectory)
                                 .filePath(it.key() + QStringLiteral(".jpg"));
        const QFileInfo info(path);
        bool thumbnailValid = isSha256Hex(expectedHash) && info.exists() &&
            info.isFile() && !info.isSymLink() && info.size() > 0 &&
            info.size() <= kMaxThumbnailBytes &&
            sha256File(path) == expectedHash;
        if (thumbnailValid) {
            QImageReader reader(path);
            thumbnailValid = reader.canRead();
        }
        if (!thumbnailValid && info.exists()) {
            QFile::remove(path);
        }

        const QString sourceContentSha256 =
            record.value(QStringLiteral("sourceContentSha256")).toString();
        const QString preparedSha256 =
            record.value(QStringLiteral("preparedSha256")).toString();
        const QString conversionProfile =
            record.value(QStringLiteral("conversionProfile")).toString();
        bool sourceSizeOk = false;
        const qint64 sourceSize =
            record.value(QStringLiteral("sourceSize")).toString().toLongLong(
                &sourceSizeOk);
        const bool originValid =
            storedVersion == kMediaCatalogFormatVersion &&
            isSha256Hex(sourceContentSha256) &&
            isSha256Hex(preparedSha256) && sourceSizeOk && sourceSize > 0 &&
            !conversionProfile.isEmpty() && conversionProfile.size() <= 256;

        record.remove(QStringLiteral("sha256"));
        if (thumbnailValid) {
            record.insert(QStringLiteral("thumbnailSha256"), expectedHash);
            record.insert(QStringLiteral("thumbnailSize"),
                          QString::number(info.size()));
        } else {
            record.remove(QStringLiteral("thumbnailSha256"));
            record.remove(QStringLiteral("thumbnailSize"));
        }
        if (!originValid) {
            record.remove(QStringLiteral("sourceContentSha256"));
            record.remove(QStringLiteral("sourceSize"));
            record.remove(QStringLiteral("conversionProfile"));
            record.remove(QStringLiteral("preparedSha256"));
            record.remove(QStringLiteral("originOperationId"));
            record.remove(QStringLiteral("originConfirmedUtc"));
        }
        if (!thumbnailValid && !originValid) {
            continue;
        }
        mediaCatalogIndex_.insert(it.key(), record);
    }
    if (storedVersion == 1) {
        const QString backupPath = QDir(catalogDirectory).filePath(
            QStringLiteral("index.v1.rollback.json"));
        bool backupReady = false;
        const QFileInfo backupInfo(backupPath);
        if (backupInfo.exists() && backupInfo.isFile() &&
            !backupInfo.isSymLink() && backupInfo.size() > 0 &&
            backupInfo.size() <= kMaxMediaCatalogIndexBytes) {
            QFile backupFile(backupPath);
            if (backupFile.open(QIODevice::ReadOnly)) {
                QJsonParseError backupParseError;
                const QJsonDocument backupDocument =
                    QJsonDocument::fromJson(backupFile.readAll(),
                                            &backupParseError);
                backupReady =
                    backupParseError.error == QJsonParseError::NoError &&
                    backupDocument.isObject() &&
                    backupDocument.toJson(QJsonDocument::Compact) ==
                        document.toJson(QJsonDocument::Compact);
            }
        } else if (!backupInfo.exists()) {
            QSaveFile backup(backupPath);
            if (backup.open(QIODevice::WriteOnly) &&
                backup.write(document.toJson(QJsonDocument::Compact)) > 0 &&
                backup.commit()) {
                backupReady = true;
            }
        }
        mediaCatalogWriteEnabled_ = backupReady;
        if (!backupReady) {
            qWarning() << "Cannot create the media catalog v1 rollback copy";
        } else {
            QString migrationError;
            if (!writeMediaCatalogIndex(&migrationError)) {
                mediaCatalogWriteEnabled_ = false;
                qWarning().noquote()
                    << QStringLiteral("Cannot migrate media catalog to v2: %1")
                           .arg(migrationError);
            }
        }
    }
    sweepMediaThumbnailOrphans();
}

bool DeviceManager::writeMediaCatalogIndex(QString *errorMessage) {
    if (!mediaCatalogWriteEnabled_) {
        if (errorMessage) {
            *errorMessage = tr(
                "Media catalog writes are disabled because the v1 rollback copy could not be created");
        }
        return false;
    }
    const QString directory = mediaCatalogDirectory();
    if (!QDir().mkpath(directory)) {
        if (errorMessage) {
            *errorMessage = tr("Cannot create the media catalog directory");
        }
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kMediaCatalogFormatVersion);
    root.insert(QStringLiteral("entries"), mediaCatalogIndex_);
    QSaveFile file(mediaCatalogIndexPath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

void DeviceManager::pruneMediaCatalogIndex(
    const TryxRuntimeMediaCatalogSnapshot &snapshot) {
    if (snapshot.deviceIdentity.isEmpty()) {
        return;
    }
    QSet<QString> authoritativeKeys;
    for (const TryxRuntimeMediaEntry &entry : snapshot.entries) {
        const QString key = mediaThumbnailKey(snapshot.deviceIdentity, entry);
        if (!key.isEmpty()) {
            authoritativeKeys.insert(key);
        }
    }

    QJsonObject prunedIndex = mediaCatalogIndex_;
    QStringList thumbnailsToRemove;
    const QStringList keys = mediaCatalogIndex_.keys();
    for (const QString &key : keys) {
        const QJsonObject record = mediaCatalogIndex_.value(key).toObject();
        if (record.value(QStringLiteral("deviceIdentity")).toString() ==
                snapshot.deviceIdentity &&
            !authoritativeKeys.contains(key)) {
            prunedIndex.remove(key);
            thumbnailsToRemove.append(
                QDir(mediaThumbnailDirectory())
                    .filePath(key + QStringLiteral(".jpg")));
        }
    }
    if (!thumbnailsToRemove.isEmpty()) {
        const QJsonObject previousIndex = mediaCatalogIndex_;
        mediaCatalogIndex_ = prunedIndex;
        QString error;
        if (!writeMediaCatalogIndex(&error)) {
            mediaCatalogIndex_ = previousIndex;
            qWarning().noquote()
                << QStringLiteral("Cannot prune media catalog index: %1")
                       .arg(error);
            return;
        }
        for (const QString &path : std::as_const(thumbnailsToRemove)) {
            QFile::remove(path);
        }
        sweepMediaThumbnailOrphans();
    }
}

void DeviceManager::sweepMediaThumbnailOrphans() {
    QDir directory(mediaThumbnailDirectory());
    const QFileInfoList files = directory.entryInfoList(
        QStringList{QStringLiteral("*.jpg")},
        QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    constexpr qsizetype kMaxThumbnailSweepEntries = 4096;
    const qsizetype boundedCount = qMin<qsizetype>(
        files.size(), kMaxThumbnailSweepEntries);
    for (qsizetype index = 0; index < boundedCount; ++index) {
        const QFileInfo &file = files.at(index);
        const QString key = file.completeBaseName();
        if (isSha256Hex(key) && !mediaCatalogIndex_.contains(key)) {
            QFile::remove(file.absoluteFilePath());
        }
    }
    if (files.size() > kMaxThumbnailSweepEntries) {
        qWarning() << "Media thumbnail orphan sweep reached its bounded entry limit";
    }
}

void DeviceManager::updateMediaCatalog(
    const QList<PrinterProtocol::MediaFile> &mediaFiles) {
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    snapshot.deviceIdentity = printerDeviceSerial_.trimmed();
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        TryxRuntimeMediaEntry entry;
        entry.name = media.name;
        entry.size = media.size;
        entry.source = media.source == PrinterProtocol::MediaSource::Preset
            ? 2U
            : 1U;
        entry.readOnly = media.readOnly;
        const QString key = mediaThumbnailKey(snapshot.deviceIdentity, entry);
        if (!key.isEmpty() && mediaCatalogIndex_.contains(key)) {
            const QJsonObject stored =
                mediaCatalogIndex_.value(key).toObject();
            if (!mediaThumbnailPath(key).isEmpty() &&
                isSha256Hex(stored.value(
                    QStringLiteral("thumbnailSha256")).toString())) {
                entry.thumbnailKey = key;
            }
            entry.managedOrigin =
                isSha256Hex(stored.value(
                    QStringLiteral("sourceContentSha256")).toString()) &&
                isSha256Hex(stored.value(
                    QStringLiteral("preparedSha256")).toString()) &&
                !stored.value(
                    QStringLiteral("conversionProfile")).toString().isEmpty();
        }
        if (entry.source == 2U) {
            entry.deleteBlockReason = QStringLiteral("Preset");
        } else if (entry.readOnly) {
            entry.deleteBlockReason = QStringLiteral("ReadOnly");
        } else if (entry.name.startsWith(
                       QStringLiteral("default_"),
                       Qt::CaseInsensitive)) {
            entry.deleteBlockReason = QStringLiteral("ProtectedName");
        } else {
            entry.deleteAllowed = true;
        }
        snapshot.entries.append(entry);
    }
    pruneMediaCatalogIndex(snapshot);
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

void DeviceManager::clearMediaCatalogView() {
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

QString DeviceManager::paseMetricsConfigDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!paseMetricsConfigDirectoryOverride_.isEmpty()) {
        return paseMetricsConfigDirectoryOverride_;
    }
#endif
    return QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
}

QString DeviceManager::paseMetricsConfigPath() const {
    return QDir(paseMetricsConfigDirectory())
        .filePath(QStringLiteral("pase-metrics.json"));
}

void DeviceManager::loadPaseMetricsConfig() {
    persistedPaseMetricsSerial_.clear();
    persistedPaseOverlay_ = {};

    const QFileInfo info(paseMetricsConfigPath());
    if (!info.exists()) {
        return;
    }
    constexpr qint64 kMaxMetricsConfigBytes = 64 * 1024;
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaxMetricsConfigBytes) {
        qWarning() << "Ignoring unsafe PASE metrics configuration";
        return;
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read PASE metrics configuration"
                   << file.errorString();
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        qWarning() << "Ignoring malformed PASE metrics configuration"
                   << parseError.errorString();
        return;
    }

    const QJsonObject root = document.object();
    const int version =
        root.value(QStringLiteral("version")).toInt(-1);
    const QString serial =
        root.value(QStringLiteral("deviceSerial")).toString().trimmed();
    if ((version != 1 &&
         version != kPaseMetricsConfigFormatVersion) ||
        !root.value(QStringLiteral("enabled")).toBool() ||
        serial.isEmpty() || serial.size() > 256) {
        qWarning() << "Ignoring unsupported PASE metrics configuration";
        return;
    }

    const auto parseArea =
        [](const QJsonObject &source,
           PrinterProtocol::PaseOverlayAreaConfig *area) {
            if (!area) {
                return false;
            }
            const QString alignment =
                source.value(QStringLiteral("alignment")).toString();
            const qint64 textColor =
                source.value(QStringLiteral("textColor")).toInteger(-1);
            const QString placement =
                source.value(QStringLiteral("verticalPlacement"))
                    .toString(QStringLiteral("Top"));
            if ((alignment != QStringLiteral("Left") &&
                 alignment != QStringLiteral("Center") &&
                 alignment != QStringLiteral("Right")) ||
                textColor < 0 ||
                textColor > 0xFFFFFF ||
                (placement != QStringLiteral("Top") &&
                 placement != QStringLiteral("Bottom"))) {
                return false;
            }
            QStringList metrics;
            QSet<QString> seenMetrics;
            const QJsonArray metricValues =
                source.value(QStringLiteral("metrics")).toArray();
            if (metricValues.size() > 3) {
                return false;
            }
            for (const QJsonValue &value : metricValues) {
                const QString metric = value.toString();
                if (!isSupportedPaseMetricLabel(metric) ||
                    seenMetrics.contains(metric)) {
                    return false;
                }
                seenMetrics.insert(metric);
                metrics.append(metric);
            }
            QStringList badges;
            QSet<QString> seenBadges;
            const QJsonArray badgeValues =
                source.value(QStringLiteral("badges")).toArray();
            if (badgeValues.size() > 2) {
                return false;
            }
            for (const QJsonValue &value : badgeValues) {
                const QString badge = value.toString();
                if (!isSupportedPaseBadge(badge) ||
                    seenBadges.contains(badge)) {
                    return false;
                }
                seenBadges.insert(badge);
                badges.append(badge);
            }
            area->metrics = metrics;
            area->badges = badges;
            area->alignment = alignment;
            area->textColor = static_cast<quint32>(textColor);
            area->verticalPlacement = placement;
            return true;
        };

    PrinterProtocol::PaseOverlayConfig overlay;
    if (version == 1) {
        QJsonObject legacyArea;
        legacyArea.insert(
            QStringLiteral("metrics"),
            root.value(QStringLiteral("metrics")).toArray());
        legacyArea.insert(
            QStringLiteral("badges"), QJsonArray{});
        legacyArea.insert(
            QStringLiteral("alignment"),
            root.value(QStringLiteral("alignment")));
        legacyArea.insert(
            QStringLiteral("textColor"),
            root.value(QStringLiteral("textColor")));
        legacyArea.insert(
            QStringLiteral("verticalPlacement"),
            QStringLiteral("Top"));
        if (!parseArea(legacyArea, &overlay.left) ||
            overlay.left.metrics.isEmpty()) {
            qWarning() << "Ignoring invalid legacy PASE metrics configuration";
            return;
        }
    } else {
        overlay.dualMode =
            root.value(QStringLiteral("dualMode")).toBool(false);
        overlay.waterfallMode =
            root.value(QStringLiteral("waterfallMode")).toBool(false);
        if (!parseArea(
                root.value(QStringLiteral("left")).toObject(),
                &overlay.left) ||
            (overlay.dualMode &&
             !parseArea(
                 root.value(QStringLiteral("right")).toObject(),
                 &overlay.right)) ||
            !paseOverlayHasContent(overlay)) {
            qWarning() << "Ignoring invalid PASE overlay configuration";
            return;
        }
    }

    persistedPaseMetricsSerial_ = serial;
    persistedPaseOverlay_ = overlay;
}

bool DeviceManager::persistPaseMetricsConfiguration(
    const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
    QString *errorMessage) {
    const QString serial = printerDeviceSerial_.trimmed();
    if (serial.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Cannot persist PASE metrics without a device serial");
        }
        return false;
    }
    if (!enabled) {
        const QString path = paseMetricsConfigPath();
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (errorMessage) {
                *errorMessage = tr(
                    "Cannot remove the previous PASE metrics configuration");
            }
            return false;
        }
        persistedPaseMetricsSerial_.clear();
        persistedPaseOverlay_ = {};
        return true;
    }

    if (!paseOverlayHasContent(overlay)) {
        if (errorMessage) {
            *errorMessage = tr(
                "Cannot persist an enabled PASE overlay without metrics or badges");
        }
        return false;
    }

    const auto areaIsValid =
        [](const PrinterProtocol::PaseOverlayAreaConfig &area) {
            if (area.metrics.size() > 3 ||
                area.badges.size() > 2 ||
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
        };
    if (!areaIsValid(overlay.left) ||
        (overlay.dualMode && !areaIsValid(overlay.right))) {
        if (errorMessage) {
            *errorMessage = tr(
                "Cannot persist an invalid PASE overlay configuration");
        }
        return false;
    }
    if (!QDir().mkpath(paseMetricsConfigDirectory())) {
        if (errorMessage) {
            *errorMessage = tr(
                "Cannot create the PASE metrics configuration directory");
        }
        return false;
    }

    const auto areaToJson =
        [](const PrinterProtocol::PaseOverlayAreaConfig &area) {
            QJsonArray metrics;
            for (const QString &metric : area.metrics) {
                metrics.append(metric);
            }
            QJsonArray badges;
            for (const QString &badge : area.badges) {
                badges.append(badge);
            }
            QJsonObject object;
            object.insert(QStringLiteral("metrics"), metrics);
            object.insert(QStringLiteral("badges"), badges);
            object.insert(QStringLiteral("alignment"),
                          area.alignment);
            object.insert(QStringLiteral("textColor"),
                          static_cast<qint64>(area.textColor));
            object.insert(QStringLiteral("verticalPlacement"),
                          area.verticalPlacement);
            return object;
        };
    QJsonObject root;
    root.insert(QStringLiteral("version"),
                kPaseMetricsConfigFormatVersion);
    root.insert(QStringLiteral("enabled"), true);
    root.insert(QStringLiteral("deviceSerial"), serial);
    root.insert(QStringLiteral("dualMode"), overlay.dualMode);
    root.insert(QStringLiteral("waterfallMode"),
                overlay.waterfallMode);
    root.insert(QStringLiteral("left"),
                areaToJson(overlay.left));
    root.insert(QStringLiteral("right"),
                areaToJson(overlay.right));

    QSaveFile file(paseMetricsConfigPath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    persistedPaseMetricsSerial_ = serial;
    persistedPaseOverlay_ = overlay;
    return true;
}

PrinterProtocol::PaseOverlayConfig
DeviceManager::persistedPaseOverlayForDevice(
    const QString &deviceSerial) const {
    if (deviceSerial.trimmed().isEmpty() ||
        deviceSerial.trimmed() != persistedPaseMetricsSerial_) {
        return {};
    }
    PrinterProtocol::PaseOverlayConfig overlay = persistedPaseOverlay_;
    overlay.left.initialLabels.clear();
    overlay.left.initialValues.clear();
    overlay.left.initialUnits.clear();
    overlay.right.initialLabels.clear();
    overlay.right.initialValues.clear();
    overlay.right.initialUnits.clear();
    overlay.cpuBadgeText.clear();
    overlay.gpuBadgeText.clear();
    return overlay;
}

QString DeviceManager::promoteThumbnailForOperation(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() || found->stagedThumbnailPath.isEmpty() ||
        !isSha256Hex(found->stagedThumbnailSha256) ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        return {};
    }

    const QFileInfo stagedInfo(found->stagedThumbnailPath);
    if (!stagedInfo.exists() || !stagedInfo.isFile() ||
        stagedInfo.isSymLink() || stagedInfo.size() <= 0 ||
        stagedInfo.size() > kMaxThumbnailBytes ||
        sha256File(found->stagedThumbnailPath) !=
            found->stagedThumbnailSha256) {
        return {};
    }
    QImageReader reader(found->stagedThumbnailPath);
    if (!reader.canRead()) {
        return {};
    }

    const QString key = mediaThumbnailKey(printerDeviceSerial_.trimmed(),
                                          verifiedEntry);
    if (key.isEmpty() || !QDir().mkpath(mediaThumbnailDirectory())) {
        return {};
    }
    const QString finalPath = QDir(mediaThumbnailDirectory())
                                  .filePath(key + QStringLiteral(".jpg"));
    const bool finalExisted = QFileInfo::exists(finalPath);
    const QJsonValue previousRecord = mediaCatalogIndex_.value(key);
    if (!previousRecord.isUndefined() && finalExisted) {
        const QJsonObject previousObject = previousRecord.toObject();
        const QString previousHash =
            previousObject.value(
                QStringLiteral("thumbnailSha256")).toString();
        const QFileInfo previousInfo(finalPath);
        QImageReader previousReader(finalPath);
        if (isSha256Hex(previousHash) && previousInfo.isFile() &&
            !previousInfo.isSymLink() && previousInfo.size() > 0 &&
            previousInfo.size() <= kMaxThumbnailBytes &&
            sha256File(finalPath) == previousHash &&
            previousReader.canRead()) {
            return key;
        }
    }
    QByteArray previousBytes;
    if (finalExisted) {
        QFile previous(finalPath);
        if (!previous.open(QIODevice::ReadOnly) ||
            previous.size() <= 0 || previous.size() > kMaxThumbnailBytes) {
            return {};
        }
        previousBytes = previous.readAll();
        if (previousBytes.size() != previous.size()) {
            return {};
        }
    }
    QFile source(found->stagedThumbnailPath);
    QSaveFile destination(finalPath);
    if (!source.open(QIODevice::ReadOnly) ||
        !destination.open(QIODevice::WriteOnly)) {
        return {};
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(256 * 1024);
        if ((chunk.isEmpty() && source.error() != QFileDevice::NoError) ||
            destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            return {};
        }
    }
    if (!destination.commit()) {
        return {};
    }

    QJsonObject record = previousRecord.isObject()
        ? previousRecord.toObject()
        : QJsonObject{};
    record.insert(QStringLiteral("deviceIdentity"),
                  printerDeviceSerial_.trimmed());
    record.insert(QStringLiteral("name"), verifiedEntry.name);
    record.insert(QStringLiteral("size"),
                  QString::number(verifiedEntry.size));
    record.insert(QStringLiteral("source"),
                  static_cast<int>(verifiedEntry.source));
    record.insert(QStringLiteral("readOnly"), verifiedEntry.readOnly);
    record.insert(QStringLiteral("thumbnailSha256"),
                  found->stagedThumbnailSha256);
    record.insert(QStringLiteral("thumbnailSize"),
                  QString::number(stagedInfo.size()));
    mediaCatalogIndex_.insert(key, record);
    QString indexError;
    if (!writeMediaCatalogIndex(&indexError)) {
        if (previousRecord.isUndefined()) {
            mediaCatalogIndex_.remove(key);
        } else {
            mediaCatalogIndex_.insert(key, previousRecord);
        }
        if (!finalExisted) {
            QFile::remove(finalPath);
        } else {
            QSaveFile rollback(finalPath);
            if (!rollback.open(QIODevice::WriteOnly) ||
                rollback.write(previousBytes) != previousBytes.size() ||
                !rollback.commit()) {
                rollback.cancelWriting();
                qWarning().noquote()
                    << QStringLiteral("Cannot restore the previous media thumbnail after index failure: %1")
                           .arg(finalPath);
            }
        }
        qWarning().noquote()
            << QStringLiteral("Cannot commit media thumbnail index: %1")
                   .arg(indexError);
        return {};
    }
    return key;
}

bool DeviceManager::persistMediaOriginForOperation(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *errorMessage) {
    const auto found = operations_.constFind(operationId);
    const QString deviceIdentity = printerDeviceSerial_.trimmed();
    if (found == operations_.constEnd() || deviceIdentity.isEmpty() ||
        !isSha256Hex(found->sourceContentSha256) ||
        !isSha256Hex(found->preparedSha256) || found->sourceSize <= 0 ||
        found->conversionProfile.isEmpty() ||
        verifiedEntry.source != 1U || verifiedEntry.readOnly ||
        verifiedEntry.name != found->remoteName) {
        if (errorMessage) {
            *errorMessage = tr(
                "Confirmed media does not have a complete origin identity");
        }
        return false;
    }
    const QString key = mediaThumbnailKey(deviceIdentity, verifiedEntry);
    if (key.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Confirmed media identity is invalid");
        }
        return false;
    }
    const QJsonObject previous =
        mediaCatalogIndex_.value(key).toObject();
    QJsonObject record = previous;
    record.insert(QStringLiteral("deviceIdentity"), deviceIdentity);
    record.insert(QStringLiteral("name"), verifiedEntry.name);
    record.insert(QStringLiteral("size"),
                  QString::number(verifiedEntry.size));
    record.insert(QStringLiteral("source"),
                  static_cast<int>(verifiedEntry.source));
    record.insert(QStringLiteral("readOnly"), verifiedEntry.readOnly);
    record.insert(QStringLiteral("confirmedUtc"),
                  QDateTime::currentDateTimeUtc().toString(
                      Qt::ISODateWithMs));
    record.insert(QStringLiteral("sourceContentSha256"),
                  found->sourceContentSha256);
    record.insert(QStringLiteral("sourceSize"),
                  QString::number(found->sourceSize));
    record.insert(QStringLiteral("conversionProfile"),
                  found->conversionProfile);
    record.insert(QStringLiteral("preparedSha256"),
                  found->preparedSha256);
    record.insert(QStringLiteral("originOperationId"), operationId);
    record.insert(QStringLiteral("originConfirmedUtc"),
                  QDateTime::currentDateTimeUtc().toString(
                      Qt::ISODateWithMs));
    mediaCatalogIndex_.insert(key, record);
    if (!writeMediaCatalogIndex(errorMessage)) {
        if (previous.isEmpty()) {
            mediaCatalogIndex_.remove(key);
        } else {
            mediaCatalogIndex_.insert(key, previous);
        }
        return false;
    }
    return true;
}

QString DeviceManager::findReusableMediaOrigin(
    const QString &sourceContentSha256,
    const QString &conversionProfile,
    const QList<PrinterProtocol::MediaFile> &mediaFiles) const {
    const QString deviceIdentity = printerDeviceSerial_.trimmed();
    if (deviceIdentity.isEmpty() ||
        !isSha256Hex(sourceContentSha256) ||
        conversionProfile.isEmpty()) {
        return {};
    }
    QStringList candidates;
    for (auto it = mediaCatalogIndex_.constBegin();
         it != mediaCatalogIndex_.constEnd(); ++it) {
        const QJsonObject record = it.value().toObject();
        if (record.value(QStringLiteral("deviceIdentity")).toString() !=
                deviceIdentity ||
            record.value(QStringLiteral("sourceContentSha256")).toString() !=
                sourceContentSha256 ||
            record.value(QStringLiteral("conversionProfile")).toString() !=
                conversionProfile ||
            !isSha256Hex(
                record.value(QStringLiteral("preparedSha256")).toString())) {
            continue;
        }
        bool sizeOk = false;
        const quint64 expectedSize =
            record.value(QStringLiteral("size")).toString().toULongLong(
                &sizeOk);
        const QString expectedName =
            record.value(QStringLiteral("name")).toString();
        if (!sizeOk || expectedSize == 0 ||
            !PrinterProtocol::isSafeUploadMediaName(expectedName)) {
            continue;
        }
        const auto exact = std::find_if(
            mediaFiles.cbegin(), mediaFiles.cend(),
            [&expectedName, expectedSize](
                const PrinterProtocol::MediaFile &media) {
                return media.name == expectedName &&
                       static_cast<quint64>(media.size) == expectedSize &&
                       media.source ==
                           PrinterProtocol::MediaSource::User &&
                       !media.readOnly;
            });
        if (exact != mediaFiles.cend()) {
            candidates.append(expectedName);
        }
    }
    candidates.removeDuplicates();
    std::sort(candidates.begin(), candidates.end());
    return candidates.isEmpty() ? QString() : candidates.constFirst();
}

void DeviceManager::publishOperation(const QString &operationId) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd()) {
        return;
    }
    const quint64 revision = ++operationRevision_;
    qInfo().noquote()
        << QStringLiteral("operation=%1 generation=%2 state=%3 stage=%4 completed=%5 total=%6")
               .arg(found->info.id)
               .arg(found->info.deviceGeneration)
               .arg(found->info.state, found->info.stage)
               .arg(found->info.completed)
               .arg(found->info.total);
    emit operationChanged(found->info, revision);
}

void DeviceManager::finishOperation(const QString &operationId,
                                    const QString &state,
                                    const QString &errorCategory,
                                    const QString &retryMode,
                                    const QString &message) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() || operationIsTerminal(found->info.state)) {
        return;
    }
    found->info.state = state;
    found->info.stage = state;
    found->info.errorCategory = errorCategory;
    found->info.retryMode = retryMode;
    found->info.message = message;
    emit requestEndPrinterForegroundOperation(
        operationId, found->info.deviceGeneration);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    if (worker_) {
        worker_->clearPrinterOperationCancellation(operationId);
    }
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::rejectOperation(const QString &operationId,
                                    const QString &kind,
                                    const QString &subject,
                                    const QString &category,
                                    const QString &message) {
    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Failed");
    record.info.stage = QStringLiteral("Rejected");
    record.info.errorCategory = category;
    record.info.subject = subject;
    record.info.message = message;
    record.info.deviceGeneration = printerGeneration_;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    publishOperation(operationId);
    pruneOperationHistory();
}

void DeviceManager::pruneOperationHistory() {
    int terminalCount = 0;
    for (const QString &operationId : std::as_const(operationOrder_)) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd() &&
            operationIsTerminal(found->info.state)) {
            ++terminalCount;
        }
    }
    while (terminalCount > kMaxTerminalOperationHistory) {
        bool removed = false;
        for (qsizetype index = 0; index < operationOrder_.size(); ++index) {
            const QString operationId = operationOrder_.at(index);
            const auto found = operations_.constFind(operationId);
            if (found == operations_.constEnd() ||
                !operationIsTerminal(found->info.state) ||
                operationId == retryCacheOperationId_) {
                continue;
            }
            operations_.remove(operationId);
            operationOrder_.removeAt(index);
            --terminalCount;
            const quint64 revision = ++operationRevision_;
            emit operationRemoved(operationId, revision);
            removed = true;
            break;
        }
        if (!removed) {
            break;
        }
    }
}

QString DeviceManager::queueUploadOperation(const QString &requestedOperationId,
                                            const QString &localPath,
                                            bool applyAfterUpload,
                                            const TryxRuntimeApplyRequest &applyRequest,
                                            bool updateMetrics,
                                            bool ensureExisting) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    const QString kind = ensureExisting
        ? QStringLiteral("EnsureMediaAndApply")
        : applyAfterUpload
            ? QStringLiteral("UploadAndApply")
            : QStringLiteral("Upload");
    const QString subject = QFileInfo(localPath).fileName();
    if (remoteMode_) {
        TryxRuntimeOperationInfo pending;
        pending.id = operationId;
        pending.kind = kind;
        pending.subject = subject;
        pending.applyAfterUpload = applyAfterUpload;
        trackRemoteOperationRequest(pending);
        if (ensureExisting) {
            remoteOperationCall(
                QStringLiteral("QueueEnsureMediaAndApply"),
                {operationId, localPath,
                 QVariant::fromValue(applyRequest)});
        } else if (applyAfterUpload) {
            remoteOperationCall(
                QStringLiteral("QueueUploadWithApply"),
                {operationId, localPath, QVariant::fromValue(applyRequest)});
        } else {
            remoteOperationCall(QStringLiteral("QueueUpload"),
                                {operationId, localPath, applyAfterUpload});
        }
        return operationId;
    }
    if (operations_.contains(operationId)) {
        return operationId;
    }

    if (!pendingRetryValidationId_.isEmpty()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("RetryCacheValidationPending"),
            tr("Stored retry media is still being validated; retry this operation when startup validation finishes"));
        return operationId;
    }

    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            operationId, kind, subject, QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, kind, subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    if (printerDeviceSerial_.trimmed().isEmpty()) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("DeviceIdentityUnavailable"),
            tr("PASE identity is unavailable; upload cannot start safely"));
        return operationId;
    }
    const QFileInfo sourceInfo(localPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() ||
        sourceInfo.isSymLink()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("InvalidSource"),
                        tr("Media file does not exist"));
        return operationId;
    }

    TryxRuntimeApplyRequest normalizedApplyRequest = applyRequest;
    if (normalizedApplyRequest.screenMode.isEmpty()) {
        normalizedApplyRequest.screenMode = QStringLiteral("Full Screen");
    }
    if (normalizedApplyRequest.playMode.isEmpty()) {
        normalizedApplyRequest.playMode = QStringLiteral("Single");
    }
    if (normalizedApplyRequest.ratio.isEmpty()) {
        normalizedApplyRequest.ratio = QStringLiteral("2:1");
    }
    if (normalizedApplyRequest.settingsColor.isEmpty()) {
        normalizedApplyRequest.settingsColor =
            QStringLiteral("#dcdcdc");
    }
    if (normalizedApplyRequest.settingsColor2.isEmpty()) {
        normalizedApplyRequest.settingsColor2 =
            normalizedApplyRequest.settingsColor;
    }
    if (applyAfterUpload) {
        normalizedApplyRequest.media.clear();
        if (normalizedApplyRequest.waterfallMode &&
            !normalizedApplyRequest.display.orientationPresent) {
            normalizedApplyRequest.display.orientationPresent = true;
            normalizedApplyRequest.display.waterfallMode = true;
        }
        const auto metricsAreValid = [](const QStringList &metrics) {
            return metrics.size() <= 3 &&
                   !hasDuplicateMetricLabels(metrics) &&
                   std::all_of(
                       metrics.cbegin(), metrics.cend(),
                       [](const QString &label) {
                           return isSupportedPaseMetricLabel(label);
                       });
        };
        const auto badgesAreValid = [](const QStringList &badges) {
            return badges.size() <= 2 &&
                   !hasDuplicateValues(badges) &&
                   std::all_of(
                       badges.cbegin(), badges.cend(),
                       [](const QString &badge) {
                           return isSupportedPaseBadge(badge);
                       });
        };
        const bool overlayRequested =
            updateMetrics || normalizedApplyRequest.replaceOverlay ||
            !normalizedApplyRequest.sysinfoLabels.isEmpty() ||
            !normalizedApplyRequest.settingsBadges.isEmpty();
        normalizedApplyRequest.replaceOverlay = overlayRequested;
        if (normalizedApplyRequest.screenMode !=
                QStringLiteral("Full Screen") ||
            (normalizedApplyRequest.playMode != QStringLiteral("Single") &&
             normalizedApplyRequest.playMode != QStringLiteral("Loop") &&
             normalizedApplyRequest.playMode != QStringLiteral("Shuffle")) ||
            normalizedApplyRequest.ratio != QStringLiteral("2:1") ||
            !metricsAreValid(normalizedApplyRequest.sysinfoLabels) ||
            !badgesAreValid(normalizedApplyRequest.settingsBadges) ||
            !isValidPaseTextColor(
                normalizedApplyRequest.settingsColor) ||
            !normalizedApplyRequest.sysinfoLabels2.isEmpty() ||
            !normalizedApplyRequest.settingsBadges2.isEmpty() ||
            normalizedApplyRequest.display.standbyPresent ||
            (normalizedApplyRequest.display.brightnessPresent &&
             (normalizedApplyRequest.display.brightness < 0 ||
              normalizedApplyRequest.display.brightness > 100))) {
            rejectOperation(
                operationId, kind, subject,
                QStringLiteral("UnsupportedConfiguration"),
                tr("PASE upload-and-apply requires one full-screen media file, a supported play mode, up to three metrics and CPU/GPU badges"));
            return operationId;
        }
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = ensureExisting
        ? QStringLiteral("Hashing")
        : QStringLiteral("Converting");
    record.info.stage = ensureExisting
        ? QStringLiteral("HashingSource")
        : QStringLiteral("Converting");
    record.info.subject = subject;
    record.info.message = ensureExisting
        ? tr("Calculating the source media content identity...")
        : tr("Preparing media for printer-class upload...");
    record.info.deviceGeneration = printerGeneration_;
    record.info.applyAfterUpload = applyAfterUpload;
    record.uploadDeviceIdentity = printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration = printerGeneration_;
    record.applyRequest = normalizedApplyRequest;
    record.updateMetrics =
        updateMetrics || normalizedApplyRequest.replaceOverlay;
    record.ensureExisting = ensureExisting;
    record.sourcePath = sourceInfo.absoluteFilePath();
    record.sourceFingerprint = sourceFingerprint(record.sourcePath);
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (ensureExisting) {
        emit requestAnalyzePrinterSource(operationId, record.sourcePath,
                                         printerGeneration_);
    } else {
        emit requestPreparePrinterMedia(operationId, devicePath,
                                        record.sourcePath, QString(),
                                        printerGeneration_);
    }
    return operationId;
}

QString DeviceManager::queueEnsureMediaAndApplyOperation(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &applyRequest) {
    return queueUploadOperation(operationId, localPath, true,
                                applyRequest, true, true);
}

QString DeviceManager::queueDeleteMediaOperation(
    const QString &requestedOperationId,
    const QStringList &fileNames) {
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    const QString kind = QStringLiteral("DeleteMedia");
    const QString subject = fileNames.join(QStringLiteral(", "));
    if (remoteMode_) {
        TryxRuntimeOperationInfo pending;
        pending.id = operationId;
        pending.kind = kind;
        pending.subject = subject;
        trackRemoteOperationRequest(pending);
        remoteOperationCall(QStringLiteral("QueueDeleteMedia"),
                            {operationId, fileNames});
        return operationId;
    }
    if (operations_.contains(operationId)) {
        return operationId;
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        QFileInfo::exists(deleteIntentPath())) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("DeleteReconciliationPending"),
            tr("A previous delete command still requires read-only reconciliation"));
        return operationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("Busy"),
                        tr("Another operation is active: %1")
                            .arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, kind, subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, kind, subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty() ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    if (fileNames.size() != 1) {
        rejectOperation(operationId, kind, subject,
                        QStringLiteral("InvalidSelection"),
                        tr("Select exactly one media file to delete safely"));
        return operationId;
    }
    QSet<QString> seenNames;
    for (const QString &fileName : fileNames) {
        const auto catalogEntry = std::find_if(
            mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
            [&fileName](const TryxRuntimeMediaEntry &entry) {
                return entry.name == fileName;
            });
        if (!PrinterProtocol::isSafeUploadMediaName(fileName) ||
            seenNames.contains(fileName) ||
            catalogEntry == mediaCatalog_.entries.cend() ||
            !catalogEntry->deleteAllowed) {
            rejectOperation(
                operationId, kind, subject,
                QStringLiteral("DeleteNotAllowed"),
                tr("Media file is not eligible for deletion: %1")
                    .arg(fileName));
            return operationId;
        }
        seenNames.insert(fileName);
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("DeletePreflight");
    record.info.subject = subject;
    record.info.message = tr(
        "Preparing a safe delete operation...");
    record.info.deviceGeneration = printerGeneration_;
    record.deleteNames = fileNames;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    QString intentError;
    if (!writeDeleteIntent(operationId,
                           QStringLiteral("Preflight"), false, 0,
                           fileNames.constFirst(), {}, &intentError)) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("PersistenceFailed"), QString(),
            tr("Cannot persist delete intent before preflight: %1")
                .arg(intentError));
        return operationId;
    }
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterDeleteMedia(
        devicePath, fileNames, operationId, deleteIntentPath(), false,
        printerGeneration_);
    return operationId;
}

QString DeviceManager::queueApplyOperation(const QString &requestedOperationId,
                                           const TryxRuntimeApplyRequest &request,
                                           bool updateMetrics) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    TryxRuntimeApplyRequest normalizedRequest = request;
    if (normalizedRequest.screenMode.isEmpty()) {
        normalizedRequest.screenMode = QStringLiteral("Full Screen");
    }
    if (normalizedRequest.playMode.isEmpty()) {
        normalizedRequest.playMode = QStringLiteral("Single");
    }
    if (normalizedRequest.ratio.isEmpty()) {
        normalizedRequest.ratio = QStringLiteral("2:1");
    }
    if (normalizedRequest.settingsColor.isEmpty()) {
        normalizedRequest.settingsColor =
            QStringLiteral("#dcdcdc");
    }
    if (normalizedRequest.settingsColor2.isEmpty()) {
        normalizedRequest.settingsColor2 =
            normalizedRequest.settingsColor;
    }
    if (normalizedRequest.waterfallMode &&
        !normalizedRequest.display.orientationPresent) {
        normalizedRequest.display.orientationPresent = true;
        normalizedRequest.display.waterfallMode = true;
    }
    const QString presetMedia =
        printerPresetMediaFile(normalizedRequest.presetId);
    if (normalizedRequest.media.isEmpty() && !presetMedia.isEmpty()) {
        normalizedRequest.media = {presetMedia};
    }
    const bool hasMediaChange = !normalizedRequest.media.isEmpty();
    const bool hasDisplayChange =
        normalizedRequest.display.brightnessPresent ||
        normalizedRequest.display.standbyPresent ||
        normalizedRequest.display.backlightPresent ||
        normalizedRequest.display.orientationPresent;
    const bool overlayRequested =
        updateMetrics || normalizedRequest.replaceOverlay ||
        !normalizedRequest.sysinfoLabels.isEmpty() ||
        !normalizedRequest.settingsBadges.isEmpty() ||
        !normalizedRequest.sysinfoLabels2.isEmpty() ||
        !normalizedRequest.settingsBadges2.isEmpty();
    normalizedRequest.replaceOverlay = overlayRequested;
    QStringList subjectMedia;
    for (const QString &mediaFile : normalizedRequest.media) {
        subjectMedia.append(printerMediaConfigName(mediaFile));
    }
    const QString subject = hasMediaChange
        ? subjectMedia.join(QStringLiteral(" + "))
        : tr("Display settings");
    if (remoteMode_) {
        TryxRuntimeOperationInfo pending;
        pending.id = operationId;
        pending.kind = QStringLiteral("Apply");
        pending.subject = subject;
        trackRemoteOperationRequest(pending);
        remoteOperationCall(normalizedRequest.replaceOverlay
                                ? QStringLiteral("QueueApplyWithMetrics")
                                : QStringLiteral("QueueApply"),
                            {operationId,
                             QVariant::fromValue(normalizedRequest)});
        return operationId;
    }
    if (operations_.contains(operationId)) {
        return operationId;
    }

    if (!pendingRetryValidationId_.isEmpty()) {
        rejectOperation(
            operationId, QStringLiteral("Apply"), subject,
            QStringLiteral("RetryCacheValidationPending"),
            tr("Stored retry media is still being validated; retry this operation when startup validation finishes"));
        return operationId;
    }

    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            operationId, QStringLiteral("Apply"), subject,
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, QStringLiteral("Apply"), subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, QStringLiteral("Apply"), subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectOperation(operationId, QStringLiteral("Apply"), subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    const auto metricsAreValid = [](const QStringList &metrics) {
        return metrics.size() <= 3 &&
               !hasDuplicateMetricLabels(metrics) &&
               std::all_of(
                   metrics.cbegin(), metrics.cend(),
                   [](const QString &label) {
                       return isSupportedPaseMetricLabel(label);
                   });
    };
    const auto badgesAreValid = [](const QStringList &badges) {
        return badges.size() <= 2 &&
               !hasDuplicateValues(badges) &&
               std::all_of(
                   badges.cbegin(), badges.cend(),
                   [](const QString &badge) {
                       return isSupportedPaseBadge(badge);
                   });
    };
    const bool fullScreen =
        normalizedRequest.screenMode == QStringLiteral("Full Screen");
    const bool splitScreen =
        normalizedRequest.screenMode ==
        QStringLiteral("Screen Splitting");
    const bool playModeValid =
        splitScreen
        ? normalizedRequest.playMode == QStringLiteral("Single")
        : normalizedRequest.playMode == QStringLiteral("Single") ||
              normalizedRequest.playMode == QStringLiteral("Loop") ||
              normalizedRequest.playMode == QStringLiteral("Shuffle");
    const bool mediaCountValid =
        !hasMediaChange ||
        (fullScreen && normalizedRequest.media.size() == 1) ||
        (splitScreen && normalizedRequest.media.size() == 2);
    const bool rightOverlayValid =
        splitScreen ||
        (normalizedRequest.sysinfoLabels2.isEmpty() &&
         normalizedRequest.settingsBadges2.isEmpty());
    if ((!hasMediaChange && !hasDisplayChange &&
         !normalizedRequest.replaceOverlay) ||
        (!fullScreen && !splitScreen) || !playModeValid ||
        !mediaCountValid ||
        normalizedRequest.ratio != QStringLiteral("2:1") ||
        !metricsAreValid(normalizedRequest.sysinfoLabels) ||
        !metricsAreValid(normalizedRequest.sysinfoLabels2) ||
        !badgesAreValid(normalizedRequest.settingsBadges) ||
        !badgesAreValid(normalizedRequest.settingsBadges2) ||
        !isValidPaseTextColor(normalizedRequest.settingsColor) ||
        !isValidPaseTextColor(normalizedRequest.settingsColor2) ||
        !rightOverlayValid ||
        normalizedRequest.display.standbyPresent ||
        (normalizedRequest.display.brightnessPresent &&
         (normalizedRequest.display.brightness < 0 ||
          normalizedRequest.display.brightness > 100))) {
        rejectOperation(
            operationId, QStringLiteral("Apply"), subject,
            QStringLiteral("UnsupportedConfiguration"),
            tr("PASE configuration requires a display change or valid full/split media, supported play mode, up to three metrics per side and CPU/GPU badges"));
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Apply");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.resultName = subject;
    record.info.message = hasMediaChange
        ? tr("Preparing to apply printer-class media...")
        : tr("Preparing to apply printer-class display settings...");
    record.info.deviceGeneration = printerGeneration_;
    record.mediaFile = hasMediaChange
        ? printerMediaConfigName(normalizedRequest.media.constFirst())
        : QString();
    record.applyRequest = normalizedRequest;
    record.updateMetrics = normalizedRequest.replaceOverlay;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterApplyMedia(devicePath, record.mediaFile,
                                  record.applyRequest,
                                  record.updateMetrics,
                                  operationId, printerGeneration_);
    return operationId;
}

QString DeviceManager::queueMetricsConfigOperation(
    const QString &requestedOperationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    const QString subject = request.enabled
        ? request.metrics.join(QStringLiteral(", "))
        : tr("Disabled");
    if (remoteMode_) {
        TryxRuntimeOperationInfo pending;
        pending.id = operationId;
        pending.kind = QStringLiteral("MetricsConfig");
        pending.subject = subject;
        trackRemoteOperationRequest(pending);
        remoteOperationCall(
            QStringLiteral("QueueMetricsConfig"),
            {operationId, QVariant::fromValue(request)});
        return operationId;
    }
    if (operations_.contains(operationId)) {
        return operationId;
    }
    if (!pendingRetryValidationId_.isEmpty()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("RetryCacheValidationPending"),
            tr("Stored retry media is still being validated; retry this operation when startup validation finishes"));
        return operationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return operationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            printerRecoveryRequired_
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return operationId;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        rejectOperation(operationId, QStringLiteral("MetricsConfig"),
                        subject, QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return operationId;
    }
    const bool alignmentValid =
        request.alignment == QStringLiteral("Left") ||
        request.alignment == QStringLiteral("Center") ||
        request.alignment == QStringLiteral("Right");
    const bool unsupportedMetric = std::any_of(
        request.metrics.cbegin(), request.metrics.cend(),
        [](const QString &label) {
            return !isSupportedPaseMetricLabel(label);
        });
    const bool requestValid = alignmentValid &&
        request.textColor <= 0x00FFFFFFU &&
        !hasDuplicateMetricLabels(request.metrics) &&
        ((request.enabled && !request.metrics.isEmpty() &&
          request.metrics.size() <= 3 && !unsupportedMetric) ||
         (!request.enabled && request.metrics.isEmpty()));
    if (!requestValid) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            QStringLiteral("UnsupportedConfiguration"),
            tr("PASE metrics configuration requires one to three unique supported metrics, or an explicit disabled state"));
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("MetricsConfig");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.message = request.enabled
        ? tr("Preparing to configure PASE metrics...")
        : tr("Preparing to disable PASE metrics...");
    record.info.deviceGeneration = printerGeneration_;
    record.metricsRequest = request;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginPrinterForegroundOperation(operationId,
                                                 printerGeneration_);
    emit requestPrinterConfigureMetrics(devicePath, request, operationId,
                                        printerGeneration_);
    return operationId;
}

QString DeviceManager::retryOperation(const QString &sourceOperationId,
                                      const QString &requestedNewOperationId) {
    const QString newOperationId =
        normalizedOperationId(requestedNewOperationId);
    if (remoteMode_) {
        const TryxRuntimeOperationInfo sourceInfo =
            operationInfo(sourceOperationId);
        TryxRuntimeOperationInfo pending;
        pending.id = newOperationId;
        pending.parentId = sourceOperationId;
        pending.kind = QStringLiteral("UploadRetry");
        pending.subject = sourceInfo.subject;
        pending.attempt = sourceInfo.attempt + 1;
        pending.applyAfterUpload = sourceInfo.applyAfterUpload;
        trackRemoteOperationRequest(pending);
        remoteOperationCall(QStringLiteral("RetryOperation"),
                            {sourceOperationId, newOperationId});
        return newOperationId;
    }
    if (operations_.contains(newOperationId)) {
        return newOperationId;
    }
    const auto source = operations_.constFind(sourceOperationId);
    if (source == operations_.constEnd() ||
        source->info.state != QStringLiteral("RetryAvailable") ||
        source->info.retryMode != QStringLiteral("PreparedMedia") ||
        source->uploadFinalizationReconciliationPending) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        QString(), QStringLiteral("RetryUnavailable"),
                        tr("This operation has no safe prepared-media retry"));
        return newOperationId;
    }
    if (!activeOperationId_.isEmpty()) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"), source->info.subject,
            QStringLiteral("Busy"),
            tr("Another operation is active: %1").arg(activeOperationId_));
        return newOperationId;
    }
    if (printerRecoveryRequired_ || printerDisplaySessionLost_ ||
        source->requiresDeviceRecovery) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            (printerRecoveryRequired_ || source->requiresDeviceRecovery)
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            printerMutationUnavailableStatusText());
        return newOperationId;
    }
    if (!printerDisplaySessionActive_) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            QStringLiteral("SessionNotReady"),
            printerMutationUnavailableStatusText());
        return newOperationId;
    }
    if (currentPrinterPath().isEmpty()) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        source->info.subject,
                        QStringLiteral("DeviceUnavailable"),
                        printerUnavailableStatusText());
        return newOperationId;
    }
    const bool identityRequired =
        source->retryMustUseNewRemoteName ||
        source->info.terminalOutcome ==
            QStringLiteral("PartialOrUnknown") ||
        source->info.terminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    const QString expectedDeviceIdentity =
        source->uploadDeviceIdentity.trimmed();
    const QString currentDeviceIdentity =
        printerDeviceSerial_.trimmed();
    if (identityRequired &&
        (expectedDeviceIdentity.isEmpty() ||
         currentDeviceIdentity.isEmpty() ||
         expectedDeviceIdentity != currentDeviceIdentity)) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            source->info.subject,
            QStringLiteral("DeviceIdentityMismatch"),
            tr("Prepared media belongs to a different or unverified PASE connection. Reconnect the original device before Retry."));
        return newOperationId;
    }
    if (sourceOperationId != retryCacheOperationId_ ||
        source->preparedPath != retryCachePreparedPath_) {
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        source->info.subject,
                        QStringLiteral("RetryUnavailable"),
                        tr("This operation is not the active retry candidate"));
        return newOperationId;
    }
    const QFileInfo preparedInfo(source->preparedPath);
    if (source->preparedPath.isEmpty() ||
        !preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() ||
        preparedInfo.size() <= 0 ||
        (source->info.total > 0 &&
         preparedInfo.size() != source->info.total) ||
        !isSha256Hex(source->preparedSha256)) {
        const QString subject = source->info.subject;
        const bool retryCacheCleared = clearRetryCache(true);
        rejectOperation(newOperationId, QStringLiteral("UploadRetry"),
                        subject,
                        retryCacheCleared
                            ? QStringLiteral("RetryCacheInvalid")
                            : QStringLiteral("RetryCacheCleanupFailed"),
                        retryCacheCleared
                            ? tr("The prepared retry cache is no longer valid")
                            : tr("The invalid retry cache could not be removed"));
        return newOperationId;
    }

    const OperationRecord sourceRecord = source.value();
    OperationRecord record = sourceRecord;
    record.info.id = newOperationId;
    record.info.parentId = sourceOperationId;
    record.info.kind = QStringLiteral("UploadRetry");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("ValidatingRetryCache");
    record.info.errorCategory.clear();
    record.info.retryMode.clear();
    record.info.message = tr("Validating prepared media before manual retry...");
    record.info.completed = 0;
    record.info.attempt = source->info.attempt + 1;
    record.info.deviceGeneration = printerGeneration_;
    record.cancelRequested = false;
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.retryValidationPending = true;
    record.retryPreflight = false;
    record.info.resultName = record.remoteName;
    operations_.insert(newOperationId, record);
    operationOrder_.append(newOperationId);
    QString cacheError;
    if (!writeRetryCache(newOperationId,
                         sourceRecord.info.terminalOutcome.isEmpty()
                             ? sourceRecord.info.errorCategory
                             : sourceRecord.info.terminalOutcome,
                         &cacheError)) {
        operations_.remove(newOperationId);
        operationOrder_.removeAll(newOperationId);
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"),
            sourceRecord.info.subject,
            QStringLiteral("RetryCacheWriteFailed"),
            tr("Cannot transfer retry-cache ownership: %1")
                .arg(cacheError));
        return newOperationId;
    }
    activeOperationId_ = newOperationId;
    publishOperation(newOperationId);
    emit requestValidatePrinterRetryCache(
        newOperationId, record.preparedPath, record.preparedSha256);
    return newOperationId;
}

void DeviceManager::cancelOperation(const QString &operationId) {
    if (remoteMode_) {
        remoteOperationCall(QStringLiteral("CancelOperation"), {operationId});
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    if (found->info.state == QStringLiteral("RetryAvailable") &&
        activeOperationId_ != operationId) {
        if (found->info.retryMode ==
            QStringLiteral("DeleteReconcile")) {
            found->info.message = tr(
                "Delete reconciliation cannot be cancelled because FileRemove may already have been sent");
            publishOperation(operationId);
            return;
        }
        if (!clearRetryCache(true)) {
            found = operations_.find(operationId);
            if (found != operations_.end()) {
                found->info.errorCategory =
                    QStringLiteral("RetryCacheCleanupFailed");
                found->info.message =
                    tr("The retry cache could not be removed; it remains available");
                publishOperation(operationId);
            }
        }
        return;
    }
    if (activeOperationId_ != operationId ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    found->cancelRequested = true;
    if (found->info.state == QStringLiteral("Converting") ||
        found->info.state == QStringLiteral("Hashing")) {
        emit requestCancelPrinterPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("UserCancelled"), QString(),
                        tr("Operation cancelled by the user"));
        return;
    }
    if (found->retryValidationPending) {
        found->info.message =
            tr("Cancelling prepared-media validation...");
        publishOperation(operationId);
        printerMediaPreparer_->cancelRetryValidation(operationId);
        return;
    }
    found->info.message = tr("Cancelling the active USB operation...");
    publishOperation(operationId);
    worker_->cancelPrinterOperation(operationId);
}

void DeviceManager::cancelForegroundForGenerationChange(
    const QString &message) {
    if (activeOperationId_.isEmpty() ||
        !operations_.contains(activeOperationId_)) {
        return;
    }
    const QString operationId = activeOperationId_;
    OperationRecord &record = operations_[operationId];
    if (record.info.state == QStringLiteral("Converting") ||
        record.info.state == QStringLiteral("Hashing")) {
        emit requestCancelPrinterPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("DeviceChanged"), QString(), message);
        return;
    }
    record.deviceChangePending = true;
    record.deviceChangeMessage = message;
    record.info.message = message;
    publishOperation(operationId);
    if (record.retryValidationPending) {
        printerMediaPreparer_->cancelRetryValidation(operationId);
    }
}

void DeviceManager::handlePreparedUploadFailure(
    const QString &operationId, const QString &message,
    PrinterProtocol::MutationOutcome outcome) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    const QString terminalOutcome = found->requiresDeviceRecovery
        ? QStringLiteral("PartialOrUnknown")
        : mutationOutcomeName(outcome);
    const bool finalizationWasProvenCommitted =
        terminalOutcome == QStringLiteral("NotStarted") &&
        found->info.terminalOutcome ==
            QStringLiteral("FinalizationUnknown") &&
        !found->uploadFinalizationReconciliationPending;
    if (terminalOutcome == QStringLiteral("PartialOrUnknown") ||
        found->info.terminalOutcome.isEmpty() ||
        finalizationWasProvenCommitted) {
        found->info.terminalOutcome = terminalOutcome;
    }
    if (finalizationWasProvenCommitted) {
        found->info.confirmedBytes = 0;
        found->info.lastConfirmedChunkIndex = -1;
    }
    if (found->info.primaryErrorCategory.isEmpty()) {
        found->info.primaryErrorCategory = terminalOutcome;
    }
    if (found->info.primaryErrorMessage.isEmpty()) {
        found->info.primaryErrorMessage = message;
    }
    if (terminalOutcome == QStringLiteral("PartialOrUnknown")) {
        found->retryMustUseNewRemoteName = true;
    }
    const QString terminalMessage = found->requiresDeviceRecovery
        ? tr("%1 Power-cycle PASE before Retry or Save; the current firmware transfer session cannot be reused safely.")
              .arg(message)
        : message;
    const bool cancellationWasProvenBeforeMutation =
        outcome == PrinterProtocol::MutationOutcome::NotStarted ||
        outcome == PrinterProtocol::MutationOutcome::Cancelled;
    if (found->cancelRequested && cancellationWasProvenBeforeMutation) {
        const QString preparedPath = found->preparedPath;
        if (!preparedPath.isEmpty() &&
            retryCachePreparedPath_ == preparedPath) {
            if (!clearRetryCache(true)) {
                found->cancelRequested = false;
                finishOperation(
                    operationId, QStringLiteral("RetryAvailable"),
                    QStringLiteral("RetryCacheCleanupFailed"),
                    QStringLiteral("PreparedMedia"),
                    tr("Cancellation was acknowledged, but the retry cache could not be removed"));
                return;
            }
        }
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Cancelled"),
                        QStringLiteral("UserCancelled"), QString(), message);
        return;
    }
    if (!found->preparedPath.isEmpty() &&
        QFileInfo::exists(found->preparedPath) &&
        isSha256Hex(found->preparedSha256)) {
        const QString preparedPath = found->preparedPath;
        QString cacheError;
        if (writeRetryCache(operationId, terminalOutcome,
                            &cacheError)) {
            emit requestReleasePrinterPreparation(preparedPath);
            if (!found->stagedThumbnailPath.isEmpty()) {
                emit requestReleasePrinterPreparation(
                    found->stagedThumbnailPath);
            }
            finishOperation(operationId,
                            QStringLiteral("RetryAvailable"),
                            terminalOutcome,
                            QStringLiteral("PreparedMedia"), terminalMessage);
            return;
        }
        const QString combinedMessage = cacheError.isEmpty()
            ? terminalMessage
            : tr("%1. Retry cache could not be saved: %2")
                  .arg(terminalMessage, cacheError);
        removePreparedFileForOperation(operationId);
        finishOperation(operationId, QStringLiteral("Failed"),
                        QStringLiteral("RetryCacheWriteFailed"),
                        QString(), combinedMessage);
        return;
    }
    const QString state = outcome == PrinterProtocol::MutationOutcome::Cancelled
        ? QStringLiteral("Cancelled")
        : QStringLiteral("Failed");
    finishOperation(operationId, state, terminalOutcome,
                    QString(), terminalMessage);
}

void DeviceManager::removePreparedFileForOperation(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    const QString path = found->preparedPath;
    if (!path.isEmpty() && retryCachePreparedPath_ == path) {
        if (!clearRetryCache(true)) {
            return;
        }
        found = operations_.find(operationId);
        if (found == operations_.end()) {
            return;
        }
    }
    const QString thumbnailPath = found->stagedThumbnailPath;
    if (!path.isEmpty()) {
        QFile::remove(path);
        emit requestReleasePrinterPreparation(path);
    }
    if (!thumbnailPath.isEmpty()) {
        QFile::remove(thumbnailPath);
        emit requestReleasePrinterPreparation(thumbnailPath);
    }
    found->preparedPath.clear();
    found->preparedSha256.clear();
    found->stagedThumbnailPath.clear();
    found->stagedThumbnailSha256.clear();
}

QString DeviceManager::retryCacheDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!retryCacheDirectoryOverride_.isEmpty()) {
        return retryCacheDirectoryOverride_;
    }
#endif
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("prepared-media"));
}

QString DeviceManager::deleteIntentPath() const {
    return QDir(mediaCatalogDirectory())
        .filePath(QStringLiteral("delete-intent.json"));
}

bool DeviceManager::writeDeleteIntent(
    const QString &operationId, const QString &stage,
    bool mayHaveStarted, int currentIndex,
    const QString &currentName, const QStringList &deletedNames,
    QString *errorMessage) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() ||
        found->deleteNames.isEmpty() || currentIndex < 0 ||
        currentIndex >= found->deleteNames.size() ||
        found->deleteNames.at(currentIndex) != currentName ||
        printerDeviceSerial_.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Delete intent metadata is incomplete");
        }
        return false;
    }
    const auto catalogEntry = std::find_if(
        mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
        [&currentName](const TryxRuntimeMediaEntry &entry) {
            return entry.name == currentName;
        });
    if (catalogEntry == mediaCatalog_.entries.cend()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Delete target is absent from the typed media catalog");
        }
        return false;
    }
    QJsonObject intent;
    intent.insert(QStringLiteral("version"),
                  kDeleteIntentFormatVersion);
    intent.insert(QStringLiteral("createdUtc"),
                  QDateTime::currentDateTimeUtc().toString(
                      Qt::ISODateWithMs));
    intent.insert(QStringLiteral("updatedUtc"),
                  QDateTime::currentDateTimeUtc().toString(
                      Qt::ISODateWithMs));
    intent.insert(QStringLiteral("operationId"), operationId);
    intent.insert(QStringLiteral("deviceIdentity"),
                  printerDeviceSerial_.trimmed());
    intent.insert(QStringLiteral("deviceGeneration"),
                  QString::number(printerGeneration_));
    intent.insert(QStringLiteral("requestedNames"),
                  stringListToJson(found->deleteNames));
    intent.insert(QStringLiteral("deletedNames"),
                  stringListToJson(deletedNames));
    intent.insert(QStringLiteral("currentIndex"), currentIndex);
    intent.insert(QStringLiteral("currentName"), currentName);
    intent.insert(QStringLiteral("currentSize"),
                  QString::number(catalogEntry->size));
    intent.insert(QStringLiteral("currentSource"),
                  static_cast<int>(catalogEntry->source));
    intent.insert(QStringLiteral("currentReadOnly"),
                  catalogEntry->readOnly);
    intent.insert(QStringLiteral("stage"), stage);
    intent.insert(QStringLiteral("mayHaveStarted"), mayHaveStarted);
    if (!writeJsonObjectAtomically(deleteIntentPath(), intent,
                                   errorMessage)) {
        return false;
    }
    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = operationId;
    return true;
}

bool DeviceManager::clearDeleteIntent(QString *errorMessage) {
    const QString path = deleteIntentPath();
    if (QFileInfo::exists(path) && !QFile::remove(path) &&
        QFileInfo::exists(path)) {
        if (errorMessage) {
            *errorMessage = tr("Cannot remove delete intent: %1")
                                .arg(path);
        }
        return false;
    }
    pendingDeleteIntent_ = {};
    pendingDeleteOperationId_.clear();
    return true;
}

void DeviceManager::loadDeleteIntent() {
    const QFileInfo info(deleteIntentPath());
    if (!info.exists()) {
        pendingDeleteIntent_ = {};
        pendingDeleteOperationId_.clear();
        return;
    }
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > kMaxRetryManifestBytes) {
        qWarning() << "Ignoring unsafe delete intent while keeping deletes blocked";
        pendingDeleteOperationId_ = QStringLiteral("invalid-delete-intent");
        return;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot read delete intent" << file.errorString();
        pendingDeleteOperationId_ = QStringLiteral("unreadable-delete-intent");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        qWarning() << "Malformed delete intent; deletes remain blocked";
        pendingDeleteOperationId_ = QStringLiteral("invalid-delete-intent");
        return;
    }
    const QJsonObject intent = document.object();
    const QString operationId =
        intent.value(QStringLiteral("operationId")).toString();
    const QString deviceIdentity =
        intent.value(QStringLiteral("deviceIdentity")).toString().trimmed();
    const QJsonArray requestedArray =
        intent.value(QStringLiteral("requestedNames")).toArray();
    const QJsonArray deletedArray =
        intent.value(QStringLiteral("deletedNames")).toArray();
    const int currentIndex =
        intent.value(QStringLiteral("currentIndex")).toInt(-1);
    const QString currentName =
        intent.value(QStringLiteral("currentName")).toString();
    QStringList requestedNames;
    QStringList deletedNames;
    QSet<QString> seen;
    bool namesValid = !requestedArray.isEmpty() &&
                      requestedArray.size() <= 64;
    for (const QJsonValue &value : requestedArray) {
        const QString name = value.toString();
        if (!PrinterProtocol::isSafeUploadMediaName(name) ||
            seen.contains(name)) {
            namesValid = false;
            break;
        }
        seen.insert(name);
        requestedNames.append(name);
    }
    for (const QJsonValue &value : deletedArray) {
        const QString name = value.toString();
        if (!requestedNames.contains(name) ||
            deletedNames.contains(name)) {
            namesValid = false;
            break;
        }
        deletedNames.append(name);
    }
    const bool valid =
        intent.value(QStringLiteral("version")).toInt(-1) ==
            kDeleteIntentFormatVersion &&
        !QUuid(operationId).isNull() && !deviceIdentity.isEmpty() &&
        namesValid && currentIndex >= 0 &&
        currentIndex < requestedNames.size() &&
        requestedNames.at(currentIndex) == currentName &&
        intent.value(QStringLiteral("mayHaveStarted")).isBool();
    if (!valid) {
        qWarning() << "Invalid delete intent; deletes remain blocked";
        pendingDeleteOperationId_ = QStringLiteral("invalid-delete-intent");
        return;
    }
    if (!intent.value(QStringLiteral("mayHaveStarted")).toBool()) {
        QString cleanupError;
        if (!clearDeleteIntent(&cleanupError)) {
            qWarning().noquote() << cleanupError;
        }
        return;
    }

    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = operationId;
    OperationRecord record;
    if (operations_.contains(operationId)) {
        record = operations_.value(operationId);
    }
    record.info.id = operationId;
    record.info.kind = QStringLiteral("DeleteMedia");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("RetryAvailable");
    record.info.errorCategory = QStringLiteral("PartialOrUnknown");
    record.info.retryMode = QStringLiteral("DeleteReconcile");
    record.info.subject = currentName;
    record.info.resultName = currentName;
    record.info.message = tr(
        "A previous delete command requires read-only FileList reconciliation");
    record.deleteNames = requestedNames;
    record.deletedNames = deletedNames;
    record.deleteReconcileOnly = true;
    if (!operations_.contains(operationId)) {
        operations_.insert(operationId, record);
        operationOrder_.append(operationId);
    } else {
        operations_[operationId] = record;
    }
    publishOperation(operationId);
}

void DeviceManager::resumePendingDeleteReconciliation() {
    if (pendingDeleteOperationId_.isEmpty() ||
        !pendingRetryValidationId_.isEmpty() ||
        !activeOperationId_.isEmpty() ||
        !printerDisplaySessionActive_ ||
        currentPrinterPath().isEmpty() ||
        !operations_.contains(pendingDeleteOperationId_)) {
        return;
    }
    const QString expectedIdentity =
        pendingDeleteIntent_.value(
            QStringLiteral("deviceIdentity")).toString().trimmed();
    if (expectedIdentity.isEmpty() ||
        expectedIdentity != printerDeviceSerial_.trimmed()) {
        return;
    }
    const QString currentName =
        pendingDeleteIntent_.value(
            QStringLiteral("currentName")).toString();
    if (!PrinterProtocol::isSafeUploadMediaName(currentName)) {
        return;
    }
    OperationRecord &record =
        operations_[pendingDeleteOperationId_];
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("ReconcilingDelete");
    record.info.retryMode.clear();
    record.info.message = tr(
        "Reconciling the previous delete command without repeating it...");
    record.info.deviceGeneration = printerGeneration_;
    record.deleteReconcileOnly = true;
    activeOperationId_ = record.info.id;
    publishOperation(record.info.id);
    emit requestBeginPrinterForegroundOperation(record.info.id,
                                                 printerGeneration_);
    emit requestPrinterDeleteMedia(
        currentPrinterPath(), QStringList{currentName}, record.info.id,
        deleteIntentPath(), true, printerGeneration_);
}

QString DeviceManager::retryCacheManifestPath() const {
    return QDir(retryCacheDirectory())
        .filePath(QStringLiteral("retry-manifest.json"));
}

bool DeviceManager::writeRetryCache(const QString &operationId,
                                    const QString &terminalOutcome,
                                    QString *errorMessage) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        if (errorMessage) {
            *errorMessage = tr("Operation metadata is no longer available");
        }
        return false;
    }
    if (found->info.terminalOutcome.isEmpty()) {
        found->info.terminalOutcome = terminalOutcome;
    }
    const QString effectiveTerminalOutcome =
        found->info.terminalOutcome;
    const bool terminalOutcomeKnown =
        effectiveTerminalOutcome == QStringLiteral("NotStarted") ||
        effectiveTerminalOutcome == QStringLiteral("Rejected") ||
        effectiveTerminalOutcome == QStringLiteral("Cancelled") ||
        effectiveTerminalOutcome == QStringLiteral("PartialOrUnknown") ||
        effectiveTerminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    if (!terminalOutcomeKnown) {
        if (errorMessage) {
            *errorMessage = tr(
                "Prepared media has an unsupported terminal outcome");
        }
        return false;
    }
    const bool outcomeRequiresDeviceIdentity =
        effectiveTerminalOutcome ==
            QStringLiteral("PartialOrUnknown") ||
        effectiveTerminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    if (outcomeRequiresDeviceIdentity &&
        found->uploadDeviceIdentity.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Prepared media is not bound to a verified PASE identity");
        }
        return false;
    }
    if (effectiveTerminalOutcome ==
        QStringLiteral("PartialOrUnknown")) {
        found->retryMustUseNewRemoteName = true;
        found->uploadFinalizationReconciliationPending = false;
    } else if (effectiveTerminalOutcome ==
               QStringLiteral("FinalizationUnknown")) {
        if (!found->uploadFinalizationReconciliationPending) {
            if (errorMessage) {
                *errorMessage = tr(
                    "Finalization recovery is not marked as reconciliation-only");
            }
            return false;
        }
    } else {
        found->uploadFinalizationReconciliationPending = false;
    }
    if (found->info.primaryErrorCategory.isEmpty()) {
        found->info.primaryErrorCategory =
            found->info.terminalOutcome.isEmpty()
                ? terminalOutcome
                : found->info.terminalOutcome;
    }
    if (found->info.primaryErrorMessage.isEmpty()) {
        found->info.primaryErrorMessage = found->info.message;
    }
    if (found->originalRemoteName.isEmpty()) {
        found->originalRemoteName = found->remoteName;
    }
    const QFileInfo preparedInfo(found->preparedPath);
    if (!preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() ||
        preparedInfo.size() <= 0 ||
        preparedInfo.size() > kMaxRetryCacheBytes ||
        !isSha256Hex(found->preparedSha256)) {
        if (errorMessage) {
            *errorMessage = tr("Prepared media failed retry-cache validation");
        }
        return false;
    }
    if (effectiveTerminalOutcome ==
        QStringLiteral("FinalizationUnknown")) {
        found->info.confirmedBytes = preparedInfo.size();
        found->info.lastConfirmedChunkIndex =
            (preparedInfo.size() - 1) / kFileTransmitChunkSize;
    }
    const bool provenBeforeMutation =
        effectiveTerminalOutcome == QStringLiteral("NotStarted") ||
        effectiveTerminalOutcome == QStringLiteral("Rejected") ||
        effectiveTerminalOutcome == QStringLiteral("Cancelled");
    if (provenBeforeMutation &&
        (found->info.confirmedBytes != 0 ||
         found->info.lastConfirmedChunkIndex != -1)) {
        if (errorMessage) {
            *errorMessage = tr(
                "Pre-mutation retry state contains confirmed upload progress");
        }
        return false;
    }
    if (found->info.confirmedBytes < 0 ||
        found->info.confirmedBytes > preparedInfo.size() ||
        (found->info.confirmedBytes == 0
             ? found->info.lastConfirmedChunkIndex != -1
             : found->info.lastConfirmedChunkIndex !=
                   (found->info.confirmedBytes - 1) /
                       kFileTransmitChunkSize)) {
        if (errorMessage) {
            *errorMessage = tr(
                "Prepared media has inconsistent confirmed progress");
        }
        return false;
    }
    const QFileInfo sourceInfo(found->sourcePath);

    const QString previousOperationId = retryCacheOperationId_;
    const QString previousPreparedPath = retryCachePreparedPath_;
    const QString previousThumbnailPath = retryCacheThumbnailPath_;
    const QString newPreparedPath = found->preparedPath;
    QString newThumbnailPath;
    QString newThumbnailHash;
    qint64 newThumbnailSize = 0;
    QString invalidThumbnailPath;
    const QFileInfo thumbnailInfo(found->stagedThumbnailPath);
    if (!found->stagedThumbnailPath.isEmpty() &&
        thumbnailInfo.exists() && thumbnailInfo.isFile() &&
        !thumbnailInfo.isSymLink() && thumbnailInfo.size() > 0 &&
        thumbnailInfo.size() <= kMaxThumbnailBytes &&
        isSha256Hex(found->stagedThumbnailSha256) &&
        sha256File(found->stagedThumbnailPath) ==
            found->stagedThumbnailSha256) {
        QImageReader reader(found->stagedThumbnailPath);
        if (reader.canRead()) {
            newThumbnailPath = found->stagedThumbnailPath;
            newThumbnailHash = found->stagedThumbnailSha256;
            newThumbnailSize = thumbnailInfo.size();
        }
    }
    if (!found->stagedThumbnailPath.isEmpty() &&
        newThumbnailPath.isEmpty()) {
        invalidThumbnailPath = found->stagedThumbnailPath;
    }

    const QString directory = retryCacheDirectory();
    if (!QDir().mkpath(directory)) {
        if (errorMessage) {
            *errorMessage = tr("Cannot create the retry-cache directory");
        }
        return false;
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), kRetryCacheFormatVersion);
    manifest.insert(QStringLiteral("createdUtc"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("operationId"), operationId);
    manifest.insert(QStringLiteral("kind"), found->info.kind);
    manifest.insert(QStringLiteral("applyAfterUpload"),
                    found->info.applyAfterUpload);
    manifest.insert(QStringLiteral("updateMetrics"),
                    found->updateMetrics);
    manifest.insert(QStringLiteral("attempt"),
                    static_cast<int>(found->info.attempt));
    manifest.insert(QStringLiteral("conversion"),
                    QStringLiteral("h264-yuv420p-2240x1080-30fps"));
    manifest.insert(QStringLiteral("sourcePath"), found->sourcePath);
    manifest.insert(QStringLiteral("sourceSize"), sourceInfo.size());
    manifest.insert(QStringLiteral("sourceFingerprint"),
                    found->sourceFingerprint);
    if (isSha256Hex(found->sourceContentSha256) &&
        found->sourceSize > 0 &&
        !found->conversionProfile.isEmpty()) {
        manifest.insert(QStringLiteral("sourceContentSha256"),
                        found->sourceContentSha256);
        manifest.insert(QStringLiteral("sourceContentSize"),
                        QString::number(found->sourceSize));
        manifest.insert(QStringLiteral("conversionProfile"),
                        found->conversionProfile);
    }
    manifest.insert(QStringLiteral("preparedPath"), found->preparedPath);
    manifest.insert(QStringLiteral("preparedSize"), preparedInfo.size());
    manifest.insert(QStringLiteral("preparedSha256"),
                    found->preparedSha256);
    manifest.insert(QStringLiteral("remoteName"), found->remoteName);
    manifest.insert(QStringLiteral("originalRemoteName"),
                    found->originalRemoteName);
    manifest.insert(QStringLiteral("retryRemoteName"), found->remoteName);
    manifest.insert(QStringLiteral("terminalOutcome"),
                    found->info.terminalOutcome);
    manifest.insert(QStringLiteral("primaryErrorCategory"),
                    found->info.primaryErrorCategory);
    manifest.insert(QStringLiteral("primaryErrorMessage"),
                    found->info.primaryErrorMessage);
    manifest.insert(QStringLiteral("confirmedBytes"),
                    QString::number(found->info.confirmedBytes));
    manifest.insert(QStringLiteral("lastConfirmedChunkIndex"),
                    found->info.lastConfirmedChunkIndex);
    manifest.insert(QStringLiteral("requiresDeviceRecovery"),
                    found->requiresDeviceRecovery);
    manifest.insert(QStringLiteral("requiresNewRemoteName"),
                    found->retryMustUseNewRemoteName);
    manifest.insert(QStringLiteral("finalizationOnlyReconciliation"),
                    found->uploadFinalizationReconciliationPending);
    manifest.insert(QStringLiteral("deviceIdentity"),
                    found->uploadDeviceIdentity.trimmed());
    manifest.insert(QStringLiteral("uploadDeviceGeneration"),
                    QString::number(found->uploadDeviceGeneration));
    manifest.insert(QStringLiteral("thumbnailStagingPath"),
                    newThumbnailPath);
    manifest.insert(QStringLiteral("thumbnailSize"), newThumbnailSize);
    manifest.insert(QStringLiteral("thumbnailSha256"), newThumbnailHash);
    if (found->info.applyAfterUpload) {
        manifest.insert(
            QStringLiteral("applyRequest"),
            runtimeApplyRequestToJson(found->applyRequest));
    }

    QSaveFile file(retryCacheManifestPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray json = QJsonDocument(manifest).toJson(
        QJsonDocument::Compact);
    if (file.write(json) != json.size() || !file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    retryCacheOperationId_ = operationId;
    retryCachePreparedPath_ = newPreparedPath;
    retryCacheThumbnailPath_ = newThumbnailPath;
    if (!invalidThumbnailPath.isEmpty()) {
        QFile::remove(invalidThumbnailPath);
        emit requestReleasePrinterPreparation(invalidThumbnailPath);
        found->stagedThumbnailPath.clear();
        found->stagedThumbnailSha256.clear();
    }

    if (!previousPreparedPath.isEmpty() &&
        previousPreparedPath != newPreparedPath) {
        if (!QFile::remove(previousPreparedPath) &&
            QFileInfo::exists(previousPreparedPath)) {
            qWarning().noquote()
                << tr("Cannot remove the superseded retry-cache file: %1")
                       .arg(previousPreparedPath);
        }
        emit requestReleasePrinterPreparation(previousPreparedPath);
    }
    if (!previousThumbnailPath.isEmpty() &&
        previousThumbnailPath != newThumbnailPath) {
        QFile::remove(previousThumbnailPath);
        emit requestReleasePrinterPreparation(previousThumbnailPath);
    }
    if (!previousOperationId.isEmpty() &&
        previousOperationId != operationId &&
        previousOperationId != activeOperationId_) {
        const auto previous = operations_.constFind(previousOperationId);
        if (previous != operations_.constEnd() &&
            previous->info.state == QStringLiteral("RetryAvailable")) {
            operations_.remove(previousOperationId);
            operationOrder_.removeAll(previousOperationId);
            emit operationRemoved(previousOperationId,
                                  ++operationRevision_);
        }
    }
    return true;
}

bool DeviceManager::clearRetryCache(bool removePreparedFile) {
    const QString manifestPath = retryCacheManifestPath();
    if (QFileInfo::exists(manifestPath) &&
        !QFile::remove(manifestPath) && QFileInfo::exists(manifestPath)) {
        const QString message =
            tr("Cannot clear the retry cache manifest: %1")
                .arg(manifestPath);
        qWarning().noquote() << message;
        emit deviceError(message);
        return false;
    }
    QStringList cleanupFailures;
    if (removePreparedFile && !retryCachePreparedPath_.isEmpty()) {
        if (!QFile::remove(retryCachePreparedPath_) &&
            QFileInfo::exists(retryCachePreparedPath_)) {
            cleanupFailures.append(retryCachePreparedPath_);
        }
        emit requestReleasePrinterPreparation(retryCachePreparedPath_);
    }
    if (removePreparedFile && !retryCacheThumbnailPath_.isEmpty()) {
        if (!QFile::remove(retryCacheThumbnailPath_) &&
            QFileInfo::exists(retryCacheThumbnailPath_)) {
            cleanupFailures.append(retryCacheThumbnailPath_);
        }
        emit requestReleasePrinterPreparation(retryCacheThumbnailPath_);
    }
    const QString operationId = retryCacheOperationId_;
    retryCachePreparedPath_.clear();
    retryCacheThumbnailPath_.clear();
    retryCacheOperationId_.clear();
    pendingRetryValidationId_.clear();
    pendingRetryManifest_ = {};
    if (!operationId.isEmpty() && operationId != activeOperationId_ &&
        operations_.contains(operationId) &&
        operations_[operationId].info.state == QStringLiteral("RetryAvailable")) {
        operations_.remove(operationId);
        operationOrder_.removeAll(operationId);
        emit operationRemoved(operationId, ++operationRevision_);
    }
    if (!cleanupFailures.isEmpty()) {
        const QString message =
            tr("The retry manifest was cleared, but cached artifacts could not be removed: %1")
                .arg(cleanupFailures.join(QStringLiteral(", ")));
        qWarning().noquote() << message;
        emit deviceError(message);
    }
    return true;
}

void DeviceManager::loadRetryCache() {
    const QString directory = retryCacheDirectory();
    QDir cacheDirectory(directory);
    if (!cacheDirectory.exists()) {
        return;
    }

    QJsonObject manifest;
    const QFileInfo manifestInfo(retryCacheManifestPath());
    if (manifestInfo.exists() && manifestInfo.isFile() &&
        !manifestInfo.isSymLink() && manifestInfo.size() > 0 &&
        manifestInfo.size() <= kMaxRetryManifestBytes) {
        QFile manifestFile(manifestInfo.absoluteFilePath());
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot read retry-cache manifest"
                       << manifestFile.errorString();
        } else {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            manifestFile.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError &&
            document.isObject()) {
            manifest = document.object();
        }
        }
    } else if (manifestInfo.exists()) {
        qWarning() << "Ignoring unsafe retry-cache manifest";
    }

    const QString preparedPath =
        manifest.value(QStringLiteral("preparedPath")).toString();
    const QFileInfo preparedInfo(preparedPath);
    const QString canonicalDirectory = QFileInfo(directory).canonicalFilePath();
    const QString canonicalPrepared = preparedInfo.canonicalFilePath();
    const bool pathIsContained = !canonicalDirectory.isEmpty() &&
        !canonicalPrepared.isEmpty() &&
        canonicalPrepared.startsWith(canonicalDirectory + QDir::separator());
    const qint64 expectedSize = static_cast<qint64>(
        manifest.value(QStringLiteral("preparedSize")).toDouble(-1));
    const QString expectedHash =
        manifest.value(QStringLiteral("preparedSha256")).toString();
    const QString storedId =
        manifest.value(QStringLiteral("operationId")).toString();
    const QString storedKind =
        manifest.value(QStringLiteral("kind")).toString();
    const QString remoteName =
        manifest.value(QStringLiteral("retryRemoteName"))
            .toString(
                manifest.value(QStringLiteral("remoteName")).toString());
    const QString sourcePath =
        manifest.value(QStringLiteral("sourcePath")).toString();
    const QString expectedSourceFingerprint =
        manifest.value(QStringLiteral("sourceFingerprint")).toString();
    const int manifestVersion =
        manifest.value(QStringLiteral("version")).toInt(-1);
    bool valid =
        (manifestVersion == 1 || manifestVersion == 2 ||
         manifestVersion == 3 || manifestVersion == 4 ||
         manifestVersion == 5 || manifestVersion == 6 ||
         manifestVersion == 7 || manifestVersion == 8 ||
         manifestVersion == kRetryCacheFormatVersion) &&
        !QUuid(storedId).isNull() &&
        (storedKind == QStringLiteral("Upload") ||
         storedKind == QStringLiteral("UploadAndApply") ||
         storedKind == QStringLiteral("EnsureMediaAndApply") ||
         storedKind == QStringLiteral("UploadRetry")) &&
        (manifestVersion < 5 ||
         manifest.value(QStringLiteral("requiresDeviceRecovery")).isBool()) &&
        PrinterProtocol::isSafeUploadMediaName(remoteName) &&
        pathIsContained && preparedInfo.isFile() &&
        !preparedInfo.isSymLink() &&
        expectedSize > 0 && expectedSize <= kMaxRetryCacheBytes &&
        preparedInfo.size() == expectedSize && isSha256Hex(expectedHash);

    const bool manifestUpdatesMetrics =
        manifestVersion == 3
            ? manifest.value(QStringLiteral("applyAfterUpload")).toBool(false)
            : manifestVersion >= 4 &&
                  manifest.value(QStringLiteral("updateMetrics"))
                      .toBool(false);
    if (valid && manifestVersion >= 8 &&
        manifest.value(QStringLiteral("applyAfterUpload")).toBool(false)) {
        TryxRuntimeApplyRequest applyRequest;
        valid =
            manifest.value(QStringLiteral("applyRequest")).isObject() &&
            runtimeApplyRequestFromJson(
                manifest.value(QStringLiteral("applyRequest")).toObject(),
                &applyRequest,
                manifestVersion >= kRetryCacheFormatVersion);
        if (valid && manifestVersion == 8 &&
            applyRequest.display.standbyPresent) {
            applyRequest.display.standbyPresent = false;
            applyRequest.display.standbyEnabled = false;
            manifest.insert(QStringLiteral("applyAfterUpload"), false);
            manifest.insert(QStringLiteral("updateMetrics"), false);
            manifest.remove(QStringLiteral("applyRequest"));
            manifest.insert(
                QStringLiteral("migrationDiagnostic"),
                tr("Prepared media was preserved, but the legacy PASE standby action was removed. Retry will upload the file without applying display settings."));
        } else {
            valid = valid &&
                    paseUploadApplyRequestIsValid(applyRequest);
        }
    } else if (valid && manifestUpdatesMetrics) {
        const QJsonArray applyMetrics =
            manifest.value(QStringLiteral("applyMetrics")).toArray();
        const QString applyAlignment =
            manifest.value(QStringLiteral("applyAlignment")).toString();
        const qint64 applyTextColor =
            manifest.value(QStringLiteral("applyTextColor")).toInteger(-1);
        QSet<QString> seenMetrics;
        valid = applyMetrics.size() <= 3 &&
                (applyAlignment == QStringLiteral("Left") ||
                 applyAlignment == QStringLiteral("Center") ||
                 applyAlignment == QStringLiteral("Right")) &&
                (applyTextColor == 0x000000 ||
                 applyTextColor == 0xDCDCDC);
        for (const QJsonValue &value : applyMetrics) {
            const QString metric = value.toString();
            if (!isSupportedPaseMetricLabel(metric) ||
                seenMetrics.contains(metric)) {
                valid = false;
                break;
            }
            seenMetrics.insert(metric);
        }
    }

    if (valid && manifestVersion >= 6) {
        const QString sourceContentSha256 =
            manifest.value(
                QStringLiteral("sourceContentSha256")).toString();
        const QString conversionProfile =
            manifest.value(
                QStringLiteral("conversionProfile")).toString();
        bool sourceContentSizeOk = false;
        const qint64 sourceContentSize =
            manifest.value(QStringLiteral("sourceContentSize"))
                .toString().toLongLong(&sourceContentSizeOk);
        const bool originEmpty = sourceContentSha256.isEmpty() &&
            conversionProfile.isEmpty() &&
            manifest.value(
                QStringLiteral("sourceContentSize")).toString().isEmpty();
        const bool originComplete =
            (isSha256Hex(sourceContentSha256) &&
             sourceContentSizeOk && sourceContentSize > 0 &&
             !conversionProfile.isEmpty() &&
             conversionProfile.size() <= 256);
        valid = (originEmpty || originComplete) &&
            (storedKind != QStringLiteral("EnsureMediaAndApply") ||
             originComplete);
    }

    if (valid && manifestVersion >= 7) {
        const QString originalRemoteName =
            manifest.value(QStringLiteral("originalRemoteName")).toString();
        const QString terminalOutcome =
            manifest.value(QStringLiteral("terminalOutcome")).toString();
        bool confirmedBytesOk = false;
        const qint64 confirmedBytes =
            manifest.value(QStringLiteral("confirmedBytes"))
                .toString().toLongLong(&confirmedBytesOk);
        bool uploadGenerationOk = false;
        manifest.value(QStringLiteral("uploadDeviceGeneration"))
            .toString().toULongLong(&uploadGenerationOk);
        const qint64 lastConfirmedChunkIndex =
            manifest.value(QStringLiteral("lastConfirmedChunkIndex"))
                .toInteger(-2);
        const bool terminalOutcomeKnown =
            terminalOutcome == QStringLiteral("NotStarted") ||
            terminalOutcome == QStringLiteral("Rejected") ||
            terminalOutcome == QStringLiteral("Cancelled") ||
            terminalOutcome == QStringLiteral("PartialOrUnknown") ||
            terminalOutcome == QStringLiteral("FinalizationUnknown");
        const bool finalizationOnly =
            manifest.value(
                QStringLiteral("finalizationOnlyReconciliation"))
                .toBool(false);
        const bool requiresNewRemoteName =
            manifest.value(QStringLiteral("requiresNewRemoteName"))
                .toBool(false);
        const QString deviceIdentity =
            manifest.value(QStringLiteral("deviceIdentity"))
                .toString().trimmed();
        const bool outcomeSafetyFlagsValid =
            terminalOutcome == QStringLiteral("PartialOrUnknown")
                ? requiresNewRemoteName && !finalizationOnly
                : terminalOutcome ==
                          QStringLiteral("FinalizationUnknown")
                    ? finalizationOnly &&
                          confirmedBytes == expectedSize &&
                          !deviceIdentity.isEmpty()
                    : !finalizationOnly &&
                          confirmedBytes == 0 &&
                          lastConfirmedChunkIndex == -1;
        const bool outcomeIdentityValid =
            terminalOutcome == QStringLiteral("PartialOrUnknown")
                ? !deviceIdentity.isEmpty()
                : true;
        valid =
            PrinterProtocol::isSafeUploadMediaName(
                originalRemoteName) &&
            manifest.value(QStringLiteral("retryRemoteName"))
                .isString() &&
            terminalOutcomeKnown &&
            manifest.value(QStringLiteral("primaryErrorCategory"))
                .isString() &&
            manifest.value(QStringLiteral("primaryErrorMessage"))
                .isString() &&
            confirmedBytesOk && confirmedBytes >= 0 &&
            confirmedBytes <= expectedSize &&
            lastConfirmedChunkIndex >= -1 &&
            (confirmedBytes == 0
                 ? lastConfirmedChunkIndex == -1
                 : lastConfirmedChunkIndex ==
                       (confirmedBytes - 1) /
                           kFileTransmitChunkSize) &&
            manifest.value(QStringLiteral("requiresNewRemoteName"))
                .isBool() &&
            manifest.value(
                QStringLiteral("finalizationOnlyReconciliation"))
                .isBool() &&
            (!finalizationOnly ||
             terminalOutcome ==
                 QStringLiteral("FinalizationUnknown")) &&
            outcomeSafetyFlagsValid &&
            outcomeIdentityValid &&
            uploadGenerationOk;
    }

    if (!valid) {
        QFile::remove(retryCacheManifestPath());
        const QFileInfoList orphanFiles = cacheDirectory.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &orphan : orphanFiles) {
            QFile::remove(orphan.absoluteFilePath());
        }
        return;
    }

    if (manifestVersion >= 2) {
        const QString thumbnailPath =
            manifest.value(QStringLiteral("thumbnailStagingPath")).toString();
        const QString thumbnailHash =
            manifest.value(QStringLiteral("thumbnailSha256")).toString();
        const qint64 thumbnailSize = static_cast<qint64>(
            manifest.value(QStringLiteral("thumbnailSize")).toDouble(0));
        bool thumbnailValid = thumbnailPath.isEmpty();
        if (!thumbnailPath.isEmpty()) {
            const QFileInfo thumbnailInfo(thumbnailPath);
            const QString canonicalThumbnail =
                thumbnailInfo.canonicalFilePath();
            const bool thumbnailPathIsContained =
                !canonicalDirectory.isEmpty() &&
                !canonicalThumbnail.isEmpty() &&
                canonicalThumbnail.startsWith(
                    canonicalDirectory + QDir::separator());
            thumbnailValid =
                thumbnailPathIsContained &&
                thumbnailInfo.isFile() && !thumbnailInfo.isSymLink() &&
                thumbnailSize > 0 &&
                thumbnailSize <= kMaxThumbnailBytes &&
                thumbnailInfo.size() == thumbnailSize &&
                isSha256Hex(thumbnailHash) &&
                sha256File(thumbnailPath) == thumbnailHash;
            if (thumbnailValid) {
                QImageReader reader(thumbnailPath);
                thumbnailValid = reader.canRead();
            }
            if (!thumbnailValid && thumbnailPathIsContained) {
                QFile::remove(thumbnailPath);
            }
        }
        if (!thumbnailValid) {
            manifest.insert(QStringLiteral("thumbnailStagingPath"),
                            QString());
            manifest.insert(QStringLiteral("thumbnailSize"), 0);
            manifest.insert(QStringLiteral("thumbnailSha256"), QString());
        }
    }

    pendingRetryValidationId_ = normalizedOperationId(storedId);
    pendingRetryManifest_ = manifest;
    emit requestValidatePrinterRetryCache(
        pendingRetryValidationId_, preparedPath, expectedHash);
}

void DeviceManager::handleRetryCacheValidation(
    const QString &validationId, bool valid, bool cancelled,
    const QString &message) {
    printerMediaPreparer_->clearRetryValidationCancellation(validationId);
    if (!pendingRetryValidationId_.isEmpty() &&
        validationId == pendingRetryValidationId_) {
        const QJsonObject manifest = pendingRetryManifest_;
        pendingRetryValidationId_.clear();
        pendingRetryManifest_ = {};
        if (cancelled) {
            qInfo().noquote()
                << tr("Stored retry-cache validation was cancelled; the cache remains available for the next runtime start");
            return;
        }
        QDir cacheDirectory(retryCacheDirectory());
        const QString preparedPath =
            manifest.value(QStringLiteral("preparedPath")).toString();
        const QString sourcePath =
            manifest.value(QStringLiteral("sourcePath")).toString();
        const QString expectedSourceFingerprint =
            manifest.value(QStringLiteral("sourceFingerprint")).toString();
        const int manifestVersion =
            manifest.value(QStringLiteral("version")).toInt(-1);
        if (!valid) {
            QFile::remove(retryCacheManifestPath());
            const QFileInfoList files = cacheDirectory.entryInfoList(
                QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo &file : files) {
                QFile::remove(file.absoluteFilePath());
            }
            qWarning().noquote()
                << (message.isEmpty()
                        ? tr("Stored retry cache failed validation")
                        : message);
            resumePrinterSessionAfterRetryCacheValidation();
            return;
        }

        const qint64 expectedSize = static_cast<qint64>(
            manifest.value(QStringLiteral("preparedSize")).toDouble(-1));
        OperationRecord record;
        record.info.id = normalizedOperationId(
            manifest.value(QStringLiteral("operationId")).toString());
        if (operations_.contains(record.info.id)) {
            qWarning().noquote()
                << tr("Stored retry operation ID conflicts with an existing operation; assigning a new ID");
            record.info.id = normalizedOperationId(QString());
        }
        record.info.kind =
            manifest.value(QStringLiteral("kind")).toString(
                QStringLiteral("Upload"));
        const QString storedTerminalOutcome =
            manifest.value(QStringLiteral("terminalOutcome")).toString();
        const bool legacyRecoveredPartial =
            manifestVersion < 7 &&
            storedTerminalOutcome == QStringLiteral("Recovered");
        const bool legacyRetryHazard =
            manifestVersion < 7 &&
            record.info.kind == QStringLiteral("UploadRetry") &&
            manifest.value(QStringLiteral("attempt")).toInt(1) > 1 &&
            storedTerminalOutcome == QStringLiteral("NotStarted");
        const bool storedTerminalOutcomeKnown =
            storedTerminalOutcome == QStringLiteral("NotStarted") ||
            storedTerminalOutcome == QStringLiteral("Rejected") ||
            storedTerminalOutcome == QStringLiteral("Cancelled") ||
            storedTerminalOutcome ==
                QStringLiteral("PartialOrUnknown") ||
            storedTerminalOutcome ==
                QStringLiteral("FinalizationUnknown");
        const bool legacyUnknownOutcome =
            manifestVersion < 7 &&
            !storedTerminalOutcomeKnown &&
            !legacyRecoveredPartial;
        const QString terminalOutcome =
            legacyRecoveredPartial || legacyRetryHazard ||
                    legacyUnknownOutcome
                ? QStringLiteral("PartialOrUnknown")
                : storedTerminalOutcome;
        const bool finalizationOnlyReconciliation =
            manifestVersion >= 7
                ? manifest
                      .value(QStringLiteral(
                          "finalizationOnlyReconciliation"))
                      .toBool(false)
                : terminalOutcome ==
                      QStringLiteral("FinalizationUnknown");
        record.info.state = finalizationOnlyReconciliation
            ? QStringLiteral("Refreshing")
            : QStringLiteral("RetryAvailable");
        record.info.stage = finalizationOnlyReconciliation
            ? QStringLiteral("RecoveringFinalization")
            : QStringLiteral("RetryAvailable");
        record.info.errorCategory = terminalOutcome;
        record.info.terminalOutcome = terminalOutcome;
        record.info.primaryErrorCategory =
            manifestVersion >= 7
                ? manifest
                      .value(QStringLiteral("primaryErrorCategory"))
                      .toString()
                : terminalOutcome;
        record.info.primaryErrorMessage =
            manifestVersion >= 7
                ? manifest
                      .value(QStringLiteral("primaryErrorMessage"))
                      .toString()
                : terminalOutcome ==
                          QStringLiteral("FinalizationUnknown")
                      ? tr("The final upload acknowledgement was lost before the file could be verified")
                      : terminalOutcome ==
                                QStringLiteral("PartialOrUnknown")
                            ? tr("The previous PASE upload ended in an unknown partial state")
                            : tr("The previous prepared upload did not complete");
        record.info.retryMode = finalizationOnlyReconciliation
            ? QString()
            : QStringLiteral("PreparedMedia");
        record.info.subject = QFileInfo(sourcePath).fileName();
        record.info.resultName =
            manifest.value(QStringLiteral("retryRemoteName"))
                .toString(
                    manifest.value(QStringLiteral("remoteName"))
                        .toString());
        record.info.total = expectedSize;
        if (manifestVersion >= 7) {
            record.info.confirmedBytes =
                manifest.value(QStringLiteral("confirmedBytes"))
                    .toString().toLongLong();
            record.info.lastConfirmedChunkIndex =
                manifest.value(
                    QStringLiteral("lastConfirmedChunkIndex"))
                    .toInteger(-1);
        } else if (terminalOutcome ==
                   QStringLiteral("FinalizationUnknown")) {
            record.info.confirmedBytes = expectedSize;
            record.info.lastConfirmedChunkIndex =
                expectedSize > 0
                    ? (expectedSize - 1) /
                          kFileTransmitChunkSize
                    : -1;
        }
        record.info.completed = record.info.confirmedBytes;
        record.info.attempt = static_cast<quint32>(qMax(
            1, manifest.value(QStringLiteral("attempt")).toInt(1)));
        record.info.deviceGeneration = printerGeneration_;
        record.info.applyAfterUpload =
            manifest.value(QStringLiteral("applyAfterUpload")).toBool(false);
        record.updateMetrics = manifestVersion == 3
            ? record.info.applyAfterUpload
            : manifestVersion >= 4 &&
                  manifest.value(QStringLiteral("updateMetrics"))
                      .toBool(false);
        if (manifestVersion >= 8 && record.info.applyAfterUpload) {
            const bool parsed = runtimeApplyRequestFromJson(
                manifest.value(QStringLiteral("applyRequest")).toObject(),
                &record.applyRequest,
                manifestVersion >= kRetryCacheFormatVersion);
            Q_ASSERT(parsed);
        } else {
            record.applyRequest.screenMode =
                QStringLiteral("Full Screen");
            record.applyRequest.playMode =
                QStringLiteral("Single");
            record.applyRequest.ratio = QStringLiteral("2:1");
        }
        if (record.updateMetrics && manifestVersion < 8) {
            const QJsonArray applyMetrics =
                manifest.value(QStringLiteral("applyMetrics")).toArray();
            for (const QJsonValue &value : applyMetrics) {
                record.applyRequest.sysinfoLabels.append(value.toString());
            }
            record.applyRequest.settingsAlign =
                manifest.value(QStringLiteral("applyAlignment")).toString();
            const qint64 applyTextColor =
                manifest.value(QStringLiteral("applyTextColor"))
                    .toInteger(0xDCDCDC);
            record.applyRequest.settingsColor =
                paseTextColorName(
                    static_cast<quint32>(
                        qBound<qint64>(
                            qint64{0}, applyTextColor,
                            qint64{0xFFFFFF})));
        }
        record.sourcePath = sourcePath;
        record.sourceFingerprint = expectedSourceFingerprint;
        if (manifestVersion >= 6) {
            record.sourceContentSha256 =
                manifest.value(
                    QStringLiteral("sourceContentSha256")).toString();
            record.sourceSize =
                manifest.value(QStringLiteral("sourceContentSize"))
                    .toString().toLongLong();
            record.conversionProfile =
                manifest.value(
                    QStringLiteral("conversionProfile")).toString();
            record.ensureExisting =
                record.info.kind ==
                QStringLiteral("EnsureMediaAndApply");
        }
        record.preparedPath = preparedPath;
        record.preparedSha256 =
            manifest.value(QStringLiteral("preparedSha256")).toString();
        record.stagedThumbnailPath =
            manifest.value(QStringLiteral("thumbnailStagingPath")).toString();
        record.stagedThumbnailSha256 =
            manifest.value(QStringLiteral("thumbnailSha256")).toString();
        record.remoteName = record.info.resultName;
        record.originalRemoteName =
            manifestVersion >= 7
                ? manifest
                      .value(QStringLiteral("originalRemoteName"))
                      .toString()
                : record.remoteName;
        record.uploadDeviceIdentity =
            manifest.value(QStringLiteral("deviceIdentity"))
                .toString().trimmed();
        if (manifestVersion >= 7) {
            record.uploadDeviceGeneration =
                manifest
                    .value(QStringLiteral("uploadDeviceGeneration"))
                    .toString().toULongLong();
            record.retryMustUseNewRemoteName =
                manifest
                    .value(QStringLiteral("requiresNewRemoteName"))
                    .toBool(false);
        } else {
            record.retryMustUseNewRemoteName =
                terminalOutcome ==
                    QStringLiteral("PartialOrUnknown") ||
                legacyRecoveredPartial ||
                (record.info.kind ==
                     QStringLiteral("UploadRetry") &&
                 record.info.attempt > 1 &&
                 terminalOutcome ==
                     QStringLiteral("NotStarted"));
        }
        const bool legacyPartialHazard =
            manifestVersion < 5 &&
            record.retryMustUseNewRemoteName;
        const bool legacyRequiresConservativeRecovery =
            legacyUnknownOutcome || legacyRetryHazard;
        record.requiresDeviceRecovery =
            (manifestVersion >= 5
                 ? manifest
                       .value(QStringLiteral("requiresDeviceRecovery"))
                       .toBool(false)
                 : legacyPartialHazard) ||
            legacyRequiresConservativeRecovery ||
            ((record.retryMustUseNewRemoteName ||
              finalizationOnlyReconciliation) &&
             record.uploadDeviceIdentity.isEmpty());
        record.uploadFinalizationReconciliationPending =
            finalizationOnlyReconciliation;
        if (finalizationOnlyReconciliation) {
            record.info.message = record.requiresDeviceRecovery
                ? tr("The completed upload still requires read-only FileList reconciliation. Physically reconnect the same PASE before verification can continue.")
                : tr("The completed upload is being recovered through read-only FileList verification; media data will not be retransmitted.");
        } else if (record.requiresDeviceRecovery) {
            record.info.message = tr(
                "The previous PASE upload ended after transmission began. Power-cycle the same device while this runtime is running before Retry.");
        } else if (record.retryMustUseNewRemoteName) {
            record.info.message = tr(
                "The same PASE was reconnected. Prepared media is available for a new transfer under a new device filename.");
        } else {
            record.info.message =
                tr("A verified prepared upload is available for manual retry");
        }
        const QString migrationDiagnostic =
            manifest.value(QStringLiteral("migrationDiagnostic"))
                .toString();
        if (!migrationDiagnostic.isEmpty()) {
            record.info.message =
                migrationDiagnostic + QLatin1Char('\n') +
                record.info.message;
            qWarning().noquote() << migrationDiagnostic;
        }
        operations_.insert(record.info.id, record);
        operationOrder_.append(record.info.id);
        retryCacheOperationId_ = record.info.id;
        retryCachePreparedPath_ = preparedPath;
        retryCacheThumbnailPath_ = record.stagedThumbnailPath;
        if (record.uploadFinalizationReconciliationPending) {
            activeOperationId_ = record.info.id;
        }
        if (manifestVersion < kRetryCacheFormatVersion) {
            QString migrationError;
            if (!writeRetryCache(record.info.id, terminalOutcome,
                                 &migrationError)) {
                qWarning().noquote()
                    << tr("Stored retry manifest could not be upgraded safely: %1")
                           .arg(migrationError);
            }
        }
        publishOperation(record.info.id);

        if (record.requiresDeviceRecovery) {
            requirePrinterRecovery(
                finalizationOnlyReconciliation
                    ? tr("A completed upload needs FileList reconciliation on the same PASE. Physically reconnect it while this runtime is running.")
                    : tr("A prepared upload was restored after an incomplete PASE transfer. Power-cycle the same device while this runtime is running before Retry."));
        } else {
            resumePrinterSessionAfterRetryCacheValidation();
        }

        const QFileInfoList orphanFiles = cacheDirectory.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &orphan : orphanFiles) {
            if (orphan.absoluteFilePath() != preparedPath &&
                orphan.absoluteFilePath() != record.stagedThumbnailPath &&
                orphan.absoluteFilePath() != retryCacheManifestPath()) {
                QFile::remove(orphan.absoluteFilePath());
            }
        }
        return;
    }

    auto found = operations_.find(validationId);
    if (found == operations_.end() ||
        activeOperationId_ != validationId ||
        !found->retryValidationPending) {
        return;
    }
    found->retryValidationPending = false;
    if (found->cancelRequested) {
        handlePreparedUploadFailure(
            validationId, tr("Operation cancelled by the user"),
            PrinterProtocol::MutationOutcome::Cancelled);
        return;
    }
    if (!valid && !cancelled) {
        const QString failure = message.isEmpty()
            ? tr("The prepared retry cache is no longer valid")
            : message;
        if (clearRetryCache(true)) {
            finishOperation(validationId, QStringLiteral("Failed"),
                            QStringLiteral("RetryCacheInvalid"), QString(),
                            failure);
        } else {
            finishOperation(
                validationId, QStringLiteral("RetryAvailable"),
                QStringLiteral("RetryCacheCleanupFailed"),
                QStringLiteral("PreparedMedia"),
                tr("%1. The retry cache could not be removed").arg(failure));
        }
        return;
    }
    if (found->deviceChangePending || currentPrinterPath().isEmpty()) {
        handlePreparedUploadFailure(
            validationId,
            found->deviceChangeMessage.isEmpty()
                ? printerUnavailableStatusText()
                : found->deviceChangeMessage,
            PrinterProtocol::MutationOutcome::NotStarted);
        return;
    }
    if (cancelled) {
        handlePreparedUploadFailure(
            validationId,
            message.isEmpty()
                ? tr("Prepared-media validation was interrupted")
                : message,
            PrinterProtocol::MutationOutcome::NotStarted);
        return;
    }
    found->info.state = QStringLiteral("Refreshing");
    found->info.stage = QStringLiteral("RefreshingMedia");
    found->info.message = tr("Checking FileList before manual retry...");
    found->retryPreflight = true;
    publishOperation(validationId);
    emit requestBeginPrinterForegroundOperation(validationId,
                                                 printerGeneration_);
    emit requestPrinterRefreshMedia(currentPrinterPath(), validationId,
                                    printerGeneration_);
}

void DeviceManager::setBrightness(int value) {
    const int boundedValue = qBound(0, value, 100);
    if (remoteMode_) {
        remoteCall(QStringLiteral("SetBrightness"), {boundedValue});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.display.brightnessPresent = true;
        request.display.brightness = boundedValue;
        queueApplyOperation(QString(), request);
        return;
    }
    emit requestBrightness(boundedValue);
}

void DeviceManager::setScreenConfig(
    const QStringList &media, const QString &ratio,
    const QString &screenMode, const QString &playMode,
    const QStringList &sysinfoLabels, const QString &settingsPosition,
    const QString &settingsColor, const QString &settingsAlign,
    const QStringList &settingsBadges, int filterOpacity,
    const QString &presetId, const QStringList &sysinfoLabels2,
    const QStringList &settingsBadges2, bool waterfallMode) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("SetScreenConfig"),
                   {media, ratio, screenMode, playMode, sysinfoLabels,
                    settingsPosition, settingsColor, settingsAlign,
                    settingsBadges, filterOpacity, presetId,
                    sysinfoLabels2, settingsBadges2, waterfallMode});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.media = media;
        request.ratio = ratio;
        request.screenMode = screenMode;
        request.playMode = playMode;
        request.sysinfoLabels = sysinfoLabels;
        request.settingsPosition = settingsPosition;
        request.settingsColor = settingsColor;
        request.settingsAlign = settingsAlign;
        request.settingsBadges = settingsBadges;
        request.filterOpacity = filterOpacity;
        request.presetId = presetId;
        request.sysinfoLabels2 = sysinfoLabels2;
        request.settingsBadges2 = settingsBadges2;
        request.waterfallMode = waterfallMode;
        request.settingsPosition2 = settingsPosition;
        request.settingsColor2 = settingsColor;
        request.settingsAlign2 = settingsAlign;
        request.replaceOverlay = true;
        request.display.orientationPresent = true;
        request.display.waterfallMode = waterfallMode;
        queueApplyOperation(QString(), request, true);
        return;
    }

    if (!connected_) {
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestScreenConfig(media, ratio, screenMode, playMode,
                             sysinfoLabels, settingsPosition, settingsColor,
                             settingsAlign, settingsBadges, filterOpacity,
                             presetId, sysinfoLabels2, settingsBadges2,
                             waterfallMode);
}

void DeviceManager::sendSysinfo(const QStringList &labels,
                                const QStringList &values,
                                const QStringList &units) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("SendSysinfo"), {labels, values, units});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty() || !printerDisplaySessionActive_ ||
            !pendingRetryValidationId_.isEmpty() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_ ||
            !activeOperationId_.isEmpty()) {
            return;
        }
        emit requestPrinterSysinfo(devicePath, labels, values, units,
                                   printerGeneration_);
        return;
    }
    emit requestSysinfo(labels, values, units);
}

void DeviceManager::setRotation(int degrees) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("SetRotation"), {degrees});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Rotation is not supported on printer-class firmware yet."));
        return;
    }
    emit requestRotation(degrees);
}

void DeviceManager::rebootDevice() {
    if (remoteMode_) {
        remoteCall(QStringLiteral("RebootDevice"));
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Reboot is not supported on printer-class firmware yet."));
        return;
    }
    emit requestReboot();
}

void DeviceManager::deleteMedia(const QStringList &files) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("DeleteMedia"), {files});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Media deletion is disabled because USB file_remove has no dedicated response."));
        return;
    }
    emit requestDeleteMedia(files);
}

void DeviceManager::uploadMedia(const QString &localPath) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("UploadMedia"), {localPath});
        return;
    }
    if (isPrinterClassDevicePresent()) {
        queueUploadOperation(QString(), localPath, false);
        return;
    }
    if (!connected_) {
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestUploadMedia(localPath);
}

void DeviceManager::refreshMediaList() {
    if (remoteMode_) {
        remoteCall(QStringLiteral("RefreshMediaList"));
        return;
    }
    if (isPrinterClassDevicePresent()) {
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty()) {
            emit deviceError(printerUnavailableStatusText());
            return;
        }
        if (!pendingRetryValidationId_.isEmpty() ||
            printerRecoveryRequired_ || printerDisplaySessionLost_) {
            emit deviceError(printerMutationUnavailableStatusText());
            return;
        }
        if (!activeOperationId_.isEmpty()) {
            emit deviceError(
                tr("FileList refresh is deferred while operation %1 is active")
                    .arg(activeOperationId_));
            return;
        }
        emit requestPrinterRefreshMedia(devicePath, QString(),
                                        printerGeneration_);
        return;
    }
    if (!connected_) {
        emit mediaListUpdated({});
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestRefreshMedia();
}

void DeviceManager::startKeepalive(int intervalSec) {
    if (remoteMode_) {
        remoteCall(QStringLiteral("StartKeepalive"), {intervalSec});
        return;
    }
    if (printerClassConnected_) {
        keepaliveTimer_->stop();
        return;
    }
    keepaliveTimer_->start(qMax(1, intervalSec) * 1000);
}

void DeviceManager::stopKeepalive() {
    if (remoteMode_) {
        remoteCall(QStringLiteral("StopKeepalive"));
        return;
    }
    keepaliveTimer_->stop();
}
