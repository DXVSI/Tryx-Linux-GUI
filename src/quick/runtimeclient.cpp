#include "runtimeclient.h"

#include "mediatransform.h"
#include "supportsnapshot.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int kRuntimeCallTimeoutMs = 5000;
constexpr int kMaximumConnectionRevisionRefreshes = 3;
constexpr int kLegacyUploadTimeoutMs = 15 * 60 * 1000;
constexpr int kDisplayApplyTimeoutMs = 2 * 60 * 1000;
constexpr int kTurrisMediaTargetWidth = 1280;
constexpr int kTurrisMediaTargetHeight = 720;
constexpr qsizetype kMaximumMetricTokenLength = 64;

bool metricsCatalogReplyIsValid(const QStringList &catalog) {
    const QStringList known = tryxMetricsCatalog();
    if (catalog.isEmpty() || catalog.size() > known.size()) {
        return false;
    }
    QSet<QString> unique;
    for (const QString &metric : catalog) {
        if (metric.isEmpty() ||
            metric.size() > kMaximumMetricTokenLength ||
            !known.contains(metric) || unique.contains(metric)) {
            return false;
        }
        unique.insert(metric);
    }
    return true;
}

QString normalizedProductCode(QString productId) {
    productId = productId.trimmed().toLower();
    const qsizetype separator = productId.lastIndexOf(QLatin1Char(':'));
    if (separator >= 0) {
        productId = productId.mid(separator + 1);
    }
    if (productId.startsWith(QStringLiteral("0x"))) {
        productId.remove(0, 2);
    }
    return productId;
}

bool isTurrisProductId(const QString &productId) {
    return normalizedProductCode(productId) == QStringLiteral("2011");
}

bool supportsDeviceSpecifications(const QString &productId) {
    const QString code = normalizedProductCode(productId);
    return code == QStringLiteral("1011") ||
           code == QStringLiteral("1021");
}

bool deviceReportedProductNameIsSafe(const QString &productName) {
    if (productName.isEmpty() || productName.size() > 128 ||
        productName != productName.trimmed()) {
        return false;
    }
    for (const uint codePoint : productName.toUcs4()) {
        const QChar::Category category = QChar::category(codePoint);
        const bool forbiddenCategory =
            category == QChar::Other_Control ||
            category == QChar::Other_Format ||
            category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph;
        const bool forbiddenCodePoint =
            codePoint <= 0x1fU ||
            (codePoint >= 0x7fU && codePoint <= 0x9fU) ||
            codePoint == 0x061cU || codePoint == 0x200eU ||
            codePoint == 0x200fU ||
            (codePoint >= 0x2028U && codePoint <= 0x202eU) ||
            (codePoint >= 0x2066U && codePoint <= 0x206fU);
        if (forbiddenCategory || forbiddenCodePoint) {
            return false;
        }
    }
    return true;
}

bool deviceSpecificationsPayloadIsEmpty(
    const TryxRuntimeDeviceSpecificationsV1 &specifications) {
    return specifications.reportedProductName.isEmpty() &&
           specifications.videoOutputWidth == 0 &&
           specifications.videoOutputHeight == 0 &&
           specifications.screenType.isEmpty() &&
           !specifications.usbAutoKeepalive;
}

bool deviceMediaArtifactMetadataIdentityIsValid(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    if (artifact.leaseId.isEmpty() ||
        artifact.leaseId != artifact.leaseId.trimmed()) {
        return false;
    }
    TryxRuntimeDeviceMediaMetadataV1 identity;
    identity.schemaVersion = artifact.schemaVersion;
    identity.operationId = artifact.operationId;
    identity.artifactId = artifact.artifactId;
    identity.mediaId = artifact.mediaId;
    identity.deviceIdentity = artifact.deviceIdentity;
    identity.decodedSha256 = artifact.decodedSha256;
    identity.deviceGeneration = 1;
    identity.status = QStringLiteral("Unavailable");
    return tryxRuntimeDeviceMediaMetadataV1IsValid(identity);
}

bool deviceMediaMetadataMatchesArtifact(
    const TryxRuntimeDeviceMediaMetadataV1 &metadata,
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    return metadata.operationId == artifact.operationId &&
           metadata.artifactId == artifact.artifactId &&
           metadata.mediaId == artifact.mediaId &&
           metadata.deviceIdentity == artifact.deviceIdentity &&
           metadata.decodedSha256 == artifact.decodedSha256;
}

QString modelForProductId(const QString &productId) {
    const QString code = normalizedProductCode(productId);
    if (code == QStringLiteral("1021") ||
        code == QStringLiteral("cm01") ||
        code == QStringLiteral("cm01_se")) {
        return QStringLiteral("PANORAMA SE");
    }
    if (code == QStringLiteral("1011")) {
        return QStringLiteral("PANORAMA");
    }
    if (code == QStringLiteral("2011")) {
        return QStringLiteral("TURRIS 620");
    }
    return {};
}

QString normalizedDeviceVersion(QString version) {
    version = version.trimmed();
    if (version.compare(QStringLiteral("unknown"),
                        Qt::CaseInsensitive) == 0) {
        return {};
    }
    return version;
}

bool isPlayMode(const QString &value, bool split) {
    return value == QStringLiteral("Single") ||
           (!split &&
            (value == QStringLiteral("Loop") ||
             value == QStringLiteral("Shuffle")));
}

QString normalizedColor(const QString &value,
                        const QString &fallback) {
    const QColor color(value);
    return color.isValid()
        ? color.name(QColor::HexRgb)
        : fallback;
}

bool isExactRgbColor(const QString &value) {
    if (value.size() != 7 || value.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (qsizetype index = 1; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (!character.isDigit() &&
            !(character >= QLatin1Char('a') &&
              character <= QLatin1Char('f')) &&
            !(character >= QLatin1Char('A') &&
              character <= QLatin1Char('F'))) {
            return false;
        }
    }
    return true;
}

bool variantStringList(const QVariant &value, QStringList *result) {
    if (!result) {
        return false;
    }
    result->clear();
    if (value.metaType().id() == QMetaType::QStringList) {
        *result = value.toStringList();
        return true;
    }
    if (value.metaType().id() != QMetaType::QVariantList) {
        return false;
    }
    const QVariantList values = value.toList();
    for (const QVariant &item : values) {
        if (item.metaType().id() != QMetaType::QString) {
            result->clear();
            return false;
        }
        result->append(item.toString());
    }
    return true;
}

bool exactBool(const QVariantMap &map, const QString &key, bool *value) {
    const auto found = map.constFind(key);
    if (found == map.constEnd() ||
        found->metaType().id() != QMetaType::Bool || !value) {
        return false;
    }
    *value = found->toBool();
    return true;
}

bool exactString(const QVariantMap &map, const QString &key,
                 QString *value) {
    const auto found = map.constFind(key);
    if (found == map.constEnd() ||
        found->metaType().id() != QMetaType::QString || !value) {
        return false;
    }
    *value = found->toString();
    return true;
}

bool exactInteger(const QVariantMap &map, const QString &key, int *value) {
    const auto found = map.constFind(key);
    if (found == map.constEnd() || !value) {
        return false;
    }
    bool ok = false;
    const double number = found->toDouble(&ok);
    if (!ok || !qIsFinite(number) || number != std::floor(number) ||
        number < std::numeric_limits<int>::min() ||
        number > std::numeric_limits<int>::max()) {
        return false;
    }
    *value = static_cast<int>(number);
    return true;
}

bool mapHasExactKeys(const QVariantMap &map,
                     const QStringList &keys) {
    return QSet<QString>(map.keyBegin(), map.keyEnd()) ==
           QSet<QString>(keys.cbegin(), keys.cend());
}

bool savedLayoutNameIsValid(const QString &name) {
    if (name.isEmpty() || name.toUcs4().size() > 80 ||
        name != name.trimmed()) {
        return false;
    }
    for (const uint codePoint : name.toUcs4()) {
        const QChar::Direction direction = QChar::direction(codePoint);
        if (QChar::category(codePoint) == QChar::Other_Control ||
            direction == QChar::DirLRE || direction == QChar::DirLRO ||
            direction == QChar::DirRLE || direction == QChar::DirRLO ||
            direction == QChar::DirPDF || direction == QChar::DirLRI ||
            direction == QChar::DirRLI || direction == QChar::DirFSI ||
            direction == QChar::DirPDI) {
            return false;
        }
    }
    return true;
}

bool savedMediaNameIsValid(const QString &name) {
    if (name.isEmpty() || name.size() > 128 ||
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

bool canonicalSavedLayoutId(const QString &layoutId) {
    const QUuid uuid(layoutId);
    return !uuid.isNull() &&
           uuid.toString(QUuid::WithoutBraces) == layoutId;
}

bool uniqueSavedValues(const QStringList &values, int maximum,
                       const QStringList &allowlist) {
    if (values.size() > maximum) {
        return false;
    }
    QSet<QString> unique;
    for (const QString &value : values) {
        if (value.isEmpty() || unique.contains(value) ||
            !allowlist.contains(value)) {
            return false;
        }
        unique.insert(value);
    }
    return true;
}

bool savedOverlayStyleIsCanonical(
    const QString &position, const QString &color,
    const QString &alignment) {
    return (position == QStringLiteral("Top") ||
            position == QStringLiteral("Bottom")) &&
           isExactRgbColor(color) && color == color.toLower() &&
           (alignment == QStringLiteral("Left") ||
            alignment == QStringLiteral("Center") ||
            alignment == QStringLiteral("Right"));
}

bool savedApplyRequestIsCanonical(
    const TryxRuntimeApplyRequest &request) {
    const bool split = request.screenMode ==
        QStringLiteral("Screen Splitting");
    const bool full = request.screenMode == QStringLiteral("Full Screen");
    const QStringList badges = {
        QStringLiteral("CPU Badge"), QStringLiteral("GPU Badge")};
    if ((!full && !split) || request.ratio != QStringLiteral("2:1") ||
        request.media.size() != (split ? 2 : 1) ||
        request.media.contains(QString()) ||
        QSet<QString>(request.media.cbegin(), request.media.cend()).size() !=
            request.media.size() ||
        !isPlayMode(request.playMode, split) ||
        !uniqueSavedValues(request.sysinfoLabels, 3, tryxMetricsCatalog()) ||
        !uniqueSavedValues(request.settingsBadges, 2, badges) ||
        !savedOverlayStyleIsCanonical(
            request.settingsPosition, request.settingsColor,
            request.settingsAlign) ||
        request.filterOpacity != 0 || !request.presetId.isEmpty() ||
        !request.replaceOverlay ||
        !request.display.brightnessPresent ||
        request.display.brightness < 0 ||
        request.display.brightness > 100 ||
        request.display.standbyPresent || request.display.standbyEnabled ||
        !request.display.orientationPresent ||
        request.display.backlightPresent ||
        !request.display.backlightEnabled ||
        request.waterfallMode != request.display.waterfallMode) {
        return false;
    }
    if (split) {
        return request.playMode == QStringLiteral("Single") &&
               uniqueSavedValues(
                   request.sysinfoLabels2, 3, tryxMetricsCatalog()) &&
               uniqueSavedValues(request.settingsBadges2, 2, badges) &&
               savedOverlayStyleIsCanonical(
                   request.settingsPosition2, request.settingsColor2,
                   request.settingsAlign2);
    }
    return request.sysinfoLabels2.isEmpty() &&
           request.settingsBadges2.isEmpty() &&
           request.settingsPosition2.isEmpty() &&
           request.settingsColor2.isEmpty() &&
           request.settingsAlign2.isEmpty();
}

QString savedLayoutMediaId(
    const QString &deviceIdentity,
    const TryxRuntimeSavedMediaRefV1 &reference) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    hash.addData(deviceIdentity.toUtf8());
    hash.addData(separator);
    hash.addData(reference.name.toUtf8());
    hash.addData(separator);
    hash.addData(QByteArray::number(reference.size));
    hash.addData(separator);
    hash.addData(QByteArray::number(reference.source));
    return QString::fromLatin1(hash.result().toHex());
}

enum class SavedLayoutMutationFailureAction {
    KeepConfirmed,
    Reconcile,
    FailClosed,
};

SavedLayoutMutationFailureAction savedLayoutMutationFailureAction(
    const QDBusError &error) {
    const QString errorName = error.name();
    if (errorName == QStringLiteral(
            "org.tryx.Panorama.Error.InvalidSavedLayout")) {
        return SavedLayoutMutationFailureAction::KeepConfirmed;
    }
    if (errorName == QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict") ||
        errorName == QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutNameConflict")) {
        return SavedLayoutMutationFailureAction::Reconcile;
    }
    switch (error.type()) {
    case QDBusError::NoReply:
    case QDBusError::Timeout:
    case QDBusError::TimedOut:
    case QDBusError::Disconnected:
        return SavedLayoutMutationFailureAction::Reconcile;
    default:
        return SavedLayoutMutationFailureAction::FailClosed;
    }
}

TryxRuntimeSavedLayoutsSnapshotV2 promoteSavedLayouts(const TryxRuntimeSavedLayoutsSnapshotV1 &old) {
    TryxRuntimeSavedLayoutsSnapshotV2 result{old.schemaVersion == 1 ? 2U : 0U, old.revision, old.status,
        old.diagnostic, old.deviceIdentity, old.productId, {}};
    for (const auto &layout : old.layouts) {
        auto promoted = tryxSavedLayoutV2FromV1(layout);
        if (layout.schemaVersion != 1) promoted.schemaVersion = 0;
        result.layouts.append(promoted);
    }
    return result;
}

// One internal lossless representation; legacy wire replies are promoted only
// when that wire version was explicitly selected before dispatch.
struct SavedLayoutsReply {
    TryxRuntimeSavedLayoutsSnapshotV2 snapshot;
    QDBusError failure;
    bool isValid() const { return !failure.isValid(); }
    const QDBusError &error() const { return failure; }
    const TryxRuntimeSavedLayoutsSnapshotV2 &value() const { return snapshot; }
};

SavedLayoutsReply savedLayoutsReply(QDBusPendingCallWatcher *watcher, bool version2) {
    if (version2) {
        const QDBusPendingReply<TryxRuntimeSavedLayoutsSnapshotV2> reply = *watcher;
        return {reply.isValid() ? reply.value() : TryxRuntimeSavedLayoutsSnapshotV2{}, reply.error()};
    }
    const QDBusPendingReply<TryxRuntimeSavedLayoutsSnapshotV1> reply = *watcher;
    return {reply.isValid() ? promoteSavedLayouts(reply.value()) : TryxRuntimeSavedLayoutsSnapshotV2{}, reply.error()};
}

bool savedLayoutIsCanonical(
    const TryxRuntimeSavedLayoutV2 &layout,
    const QString &deviceIdentity, const QString &productId) {
    TryxRuntimeOverlayBadgesV1 normalized;
    if (layout.schemaVersion != 2U || layout.revision == 0 ||
        !canonicalSavedLayoutId(layout.layoutId) ||
        !tryxSavedLayoutDeviceIdentityIsCanonical(deviceIdentity) ||
        !tryxSavedLayoutProductIdIsSupported(productId) ||
        layout.deviceIdentity != deviceIdentity ||
        layout.productId != productId ||
        !savedLayoutNameIsValid(layout.name) ||
        !savedApplyRequestIsCanonical(layout.request) ||
        layout.media.size() != layout.request.media.size()
        || !tryxNormalizeOverlayBadgesV1(layout.badges, layout.request.settingsBadges, layout.request.settingsBadges2,
            layout.request.screenMode == QStringLiteral("Screen Splitting"), &normalized)
        || normalized != layout.badges
        || (tryxOverlayBadgesHaveCustomText(layout.badges) && productId != QStringLiteral("391a:1021"))) {
        return false;
    }
    for (qsizetype index = 0; index < layout.media.size(); ++index) {
        const TryxRuntimeSavedMediaRefV1 &reference = layout.media.at(index);
        if (reference.schemaVersion != 1U || reference.size == 0 ||
            (reference.source != 1U && reference.source != 2U) ||
            !savedMediaNameIsValid(reference.name) ||
            reference.name != layout.request.media.at(index) ||
            reference.mediaId != savedLayoutMediaId(
                deviceIdentity, reference)) {
            return false;
        }
    }
    return true;
}

bool savedLayoutsSnapshotIsValid(
    const TryxRuntimeSavedLayoutsSnapshotV2 &snapshot) {
    const QStringList statuses = {
        QStringLiteral("Ready"), QStringLiteral("Disconnected"),
        QStringLiteral("Unsupported"), QStringLiteral("Unavailable")};
    if (snapshot.schemaVersion != 2U ||
        !statuses.contains(snapshot.status) ||
        snapshot.diagnostic.size() > 512 ||
        snapshot.layouts.size() > 32) {
        return false;
    }
    if (snapshot.status != QStringLiteral("Ready")) {
        return snapshot.layouts.isEmpty() &&
               (snapshot.status != QStringLiteral("Unavailable") ||
                !snapshot.diagnostic.isEmpty());
    }
    if (!tryxSavedLayoutDeviceIdentityIsCanonical(
            snapshot.deviceIdentity) ||
        !tryxSavedLayoutProductIdIsSupported(snapshot.productId) ||
        !snapshot.diagnostic.isEmpty()) {
        return false;
    }
    QSet<QString> ids;
    QSet<QString> names;
    QSet<quint64> revisions;
    for (const TryxRuntimeSavedLayoutV2 &layout : snapshot.layouts) {
        const QString folded = layout.name.toCaseFolded();
        if (ids.contains(layout.layoutId) || names.contains(folded) ||
            revisions.contains(layout.revision) ||
            layout.revision > snapshot.revision ||
            !savedLayoutIsCanonical(
                layout, snapshot.deviceIdentity, snapshot.productId)) {
            return false;
        }
        ids.insert(layout.layoutId);
        names.insert(folded);
        revisions.insert(layout.revision);
    }
    return true;
}

const TryxRuntimeSavedLayoutV2 *savedLayoutById(
    const TryxRuntimeSavedLayoutsSnapshotV2 &snapshot,
    const QString &layoutId) {
    const auto found = std::find_if(
        snapshot.layouts.cbegin(), snapshot.layouts.cend(),
        [&layoutId](const TryxRuntimeSavedLayoutV2 &layout) {
            return layout.layoutId == layoutId;
        });
    return found == snapshot.layouts.cend() ? nullptr : &*found;
}

bool unchangedSavedLayoutsArePreserved(
    const TryxRuntimeSavedLayoutsSnapshotV2 &before,
    const TryxRuntimeSavedLayoutsSnapshotV2 &after,
    const QString &changedLayoutId) {
    for (const TryxRuntimeSavedLayoutV2 &layout : before.layouts) {
        if (layout.layoutId == changedLayoutId) {
            continue;
        }
        const TryxRuntimeSavedLayoutV2 *confirmed =
            savedLayoutById(after, layout.layoutId);
        if (!confirmed || !(*confirmed == layout)) {
            return false;
        }
    }
    return true;
}

bool savedLayoutPutReplyIsExact(
    const TryxRuntimeSavedLayoutsSnapshotV2 &before,
    const TryxRuntimeSavedLayoutV2 &submitted,
    const TryxRuntimeSavedLayoutsSnapshotV2 &after,
    TryxRuntimeSavedLayoutV2 *confirmed) {
    if (before.revision == std::numeric_limits<quint64>::max() ||
        after.revision != before.revision + 1 ||
        after.status != QStringLiteral("Ready") ||
        after.deviceIdentity != before.deviceIdentity ||
        after.productId != before.productId) {
        return false;
    }
    const bool create = submitted.layoutId.isEmpty();
    if (!create && !savedLayoutById(before, submitted.layoutId)) {
        return false;
    }
    if (after.layouts.size() !=
        before.layouts.size() + (create ? 1 : 0)) {
        return false;
    }
    const TryxRuntimeSavedLayoutV2 *stored = nullptr;
    if (create) {
        for (const TryxRuntimeSavedLayoutV2 &layout : after.layouts) {
            if (layout.name.compare(
                    submitted.name, Qt::CaseInsensitive) == 0) {
                if (stored) {
                    return false;
                }
                stored = &layout;
            }
        }
    } else {
        stored = savedLayoutById(after, submitted.layoutId);
    }
    if (!stored || stored->revision != after.revision ||
        stored->schemaVersion != submitted.schemaVersion ||
        (!create && stored->layoutId != submitted.layoutId) ||
        stored->deviceIdentity != submitted.deviceIdentity ||
        stored->productId != submitted.productId ||
        stored->name != submitted.name ||
        stored->media != submitted.media ||
        !(stored->request == submitted.request) ||
        stored->badges != submitted.badges ||
        !unchangedSavedLayoutsArePreserved(
            before, after, submitted.layoutId)) {
        return false;
    }
    if (confirmed) {
        *confirmed = *stored;
    }
    return true;
}

bool savedLayoutDeleteReplyIsExact(
    const TryxRuntimeSavedLayoutsSnapshotV2 &before,
    const QString &deletedLayoutId,
    const TryxRuntimeSavedLayoutsSnapshotV2 &after) {
    return before.revision != std::numeric_limits<quint64>::max() &&
           after.revision == before.revision + 1 &&
           after.status == QStringLiteral("Ready") &&
           after.deviceIdentity == before.deviceIdentity &&
           after.productId == before.productId &&
           savedLayoutById(before, deletedLayoutId) &&
           !savedLayoutById(after, deletedLayoutId) &&
           after.layouts.size() + 1 == before.layouts.size() &&
           unchangedSavedLayoutsArePreserved(
               before, after, deletedLayoutId);
}

}  // namespace

RuntimeClient::RuntimeClient(bool offline, QObject *parent)
    : QObject(parent),
      bus_(offline
               ? QDBusConnection(
                     QStringLiteral(
                         "tryx-panorama-manager-offline"))
               : QDBusConnection::sessionBus()),
      serviceWatcher_(
          tryxRuntimeServiceName(), bus_,
          QDBusServiceWatcher::WatchForRegistration |
              QDBusServiceWatcher::WatchForUnregistration,
          this),
      savedLayoutModel_(this),
      mediaModel_(this),
      operationModel_(this),
      offline_(offline) {
    legacyUploadDeadline_.setSingleShot(true);
    legacyUploadDeadline_.setInterval(kLegacyUploadTimeoutMs);
    connect(&legacyUploadDeadline_, &QTimer::timeout,
            this, &RuntimeClient::onLegacyUploadTimeout);
    displayApplyDeadline_.setSingleShot(true);
    displayApplyDeadline_.setInterval(kDisplayApplyTimeoutMs);
    connect(&displayApplyDeadline_, &QTimer::timeout,
            this, &RuntimeClient::onDisplayApplyTimeout);
    connect(
        this, &RuntimeClient::operationRequestRejected,
        this,
        [this](const QString &operationId, const QString &,
               const QString &message) {
            if (displaySubmission_.active() &&
                displaySubmission_.id == operationId &&
                !displaySubmission_.resultEmitted) {
                finishDisplaySubmission(
                    QStringLiteral("Rejected"), message);
            }
        });
    connect(
        this, &RuntimeClient::runtimeInvalidated,
        this, [this]() {
            if (displaySubmission_.active() &&
                !displaySubmission_.resultEmitted) {
                finishDisplaySubmission(
                    QStringLiteral("Unresolved"),
                    tr("The runtime changed before the display apply result was confirmed"),
                    true);
            }
        });
    if (offline) {
        return;
    }
    registerTryxRuntimeMetaTypes();
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceRegistered,
            this, &RuntimeClient::onServiceRegistered);
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered,
            this, &RuntimeClient::onServiceUnregistered);
    subscribeSignals();

    if (!bus_.isConnected() || !bus_.interface()) {
        setDiagnostic(tr("The D-Bus session bus is unavailable"));
        return;
    }
    serviceAvailable_ =
        bus_.interface()
            ->isServiceRegistered(tryxRuntimeServiceName());
    if (serviceAvailable_) {
        startHandshake();
    }
}

bool RuntimeClient::serviceAvailable() const {
    return serviceAvailable_;
}

bool RuntimeClient::compatible() const {
    return compatible_;
}

bool RuntimeClient::connected() const {
    return connection_.connected;
}

bool RuntimeClient::ready() const {
    return serviceAvailable_ && compatible_ &&
           (legacyConnected() ||
            (connection_.printerClassDevicePresent &&
             connection_.displaySessionActive));
}

bool RuntimeClient::legacyConnected() const {
    return connection_.connected &&
           !connection_.printerClassConnected &&
           !connection_.printerClassDevicePresent;
}

bool RuntimeClient::printerClassDevicePresent() const {
    return connection_.printerClassDevicePresent;
}

bool RuntimeClient::displaySessionActive() const {
    return connection_.displaySessionActive;
}

QString RuntimeClient::productId() const {
    return connection_.productId;
}

QString RuntimeClient::deviceModel() const {
    return modelForProductId(connection_.productId);
}

QString RuntimeClient::firmwareVersion() const {
    return normalizedDeviceVersion(connection_.firmware);
}

QString RuntimeClient::deviceAppVersion() const {
    return normalizedDeviceVersion(connection_.appVersion);
}

int RuntimeClient::mediaTargetWidth() const {
    return isTurrisProductId(connection_.productId)
        ? kTurrisMediaTargetWidth
        : kTryxMediaTargetWidth;
}

int RuntimeClient::mediaTargetHeight() const {
    return isTurrisProductId(connection_.productId)
        ? kTurrisMediaTargetHeight
        : kTryxMediaTargetHeight;
}

QString RuntimeClient::connectionStatus() const {
    if (!serviceAvailable_) {
        return tr("Runtime service is not running");
    }
    if (!compatible_) {
        return tr("Runtime API is incompatible");
    }
    if (legacyConnected()) {
        return tr("Legacy serial/ADB device is connected");
    }
    if (!connection_.printerClassDevicePresent) {
        return tr("TRYX printer-class device is not present");
    }
    if (!connection_.displaySessionActive) {
        return tr("TRYX device is present, but the display session is not ready");
    }
    return tr("TRYX display session is active");
}

QString RuntimeClient::diagnostic() const {
    return diagnostic_;
}

quint32 RuntimeClient::apiVersion() const {
    return apiVersion_;
}

quint32 RuntimeClient::expectedApiVersion() const {
    return tryxRuntimeApiVersion();
}

bool RuntimeClient::capabilitiesReady() const {
    return capabilitiesReady_;
}

QStringList RuntimeClient::runtimeCapabilities() const {
    return runtimeCapabilities_;
}

bool RuntimeClient::supportSnapshotAvailable() const {
    return serviceAvailable_ && compatible_ && capabilitiesReady_ &&
           runtimeCapabilities_.contains(
               tryxRuntimeSupportSnapshotV1Token());
}

bool RuntimeClient::supportSnapshotBusy() const {
    return supportSnapshotBusy_;
}

bool RuntimeClient::deviceCapabilitiesReady() const {
    return deviceCapabilitiesReady_;
}

QStringList RuntimeClient::deviceCapabilities() const {
    return deviceCapabilities_;
}

bool RuntimeClient::deviceSpecificationsSupported() const {
    return capabilitiesReady_ &&
           runtimeCapabilities_.contains(
               tryxRuntimeDeviceSpecificationsV1Token());
}

QString RuntimeClient::deviceSpecificationsStatus() const {
    switch (deviceSpecificationsState_) {
    case DeviceSpecificationsState::NotSupported:
        return QStringLiteral("NotSupported");
    case DeviceSpecificationsState::RuntimeUnavailable:
        return QStringLiteral("RuntimeUnavailable");
    case DeviceSpecificationsState::Disconnected:
        return QStringLiteral("Disconnected");
    case DeviceSpecificationsState::Unsupported:
        return QStringLiteral("Unsupported");
    case DeviceSpecificationsState::Unavailable:
        return QStringLiteral("Unavailable");
    case DeviceSpecificationsState::Ready:
        return QStringLiteral("Ready");
    }
    return QStringLiteral("RuntimeUnavailable");
}

bool RuntimeClient::deviceSpecificationsReady() const {
    return deviceSpecificationsState_ ==
           DeviceSpecificationsState::Ready;
}

QString RuntimeClient::deviceReportedProductName() const {
    return deviceSpecificationsReady()
        ? deviceSpecificationsSnapshot_.reportedProductName
        : QString();
}

int RuntimeClient::deviceVideoOutputWidth() const {
    return deviceSpecificationsReady()
        ? static_cast<int>(
              deviceSpecificationsSnapshot_.videoOutputWidth)
        : 0;
}

int RuntimeClient::deviceVideoOutputHeight() const {
    return deviceSpecificationsReady()
        ? static_cast<int>(
              deviceSpecificationsSnapshot_.videoOutputHeight)
        : 0;
}

QString RuntimeClient::deviceScreenType() const {
    return deviceSpecificationsReady()
        ? deviceSpecificationsSnapshot_.screenType
        : QString();
}

bool RuntimeClient::deviceUsbAutoKeepalive() const {
    return deviceSpecificationsReady() &&
           deviceSpecificationsSnapshot_.usbAutoKeepalive;
}

bool RuntimeClient::presentationPreferencesReady() const {
    return presentationPreferencesReady_;
}

bool RuntimeClient::presentationPreferencesBusy() const {
    return presentationPreferencesBusy_;
}

QString RuntimeClient::temperatureUnit() const {
    return presentationPreferences_.temperatureUnit;
}

QString RuntimeClient::timeFormat() const {
    return presentationPreferences_.timeFormat;
}

SavedLayoutListModel *RuntimeClient::savedLayoutModel() {
    return &savedLayoutModel_;
}

bool RuntimeClient::savedLayoutsSupported() const {
    return capabilitiesReady_ && (runtimeCapabilities_.contains(tryxRuntimeSavedLayoutsV1Token()) || savedLayoutsV2Supported());
}

bool RuntimeClient::savedLayoutsV2Supported() const {
    return capabilitiesReady_ && runtimeCapabilities_.contains(tryxRuntimeSavedLayoutsV2Token());
}

bool RuntimeClient::savedLayoutsReady() const {
    return savedLayoutsSupported() &&
           savedLayouts_.status == QStringLiteral("Ready")
        && (savedLayoutsV2Supported() || std::none_of(savedLayouts_.layouts.cbegin(), savedLayouts_.layouts.cend(),
            [](const auto &layout) { return tryxOverlayBadgesHaveCustomText(layout.badges); }));
}

bool RuntimeClient::savedLayoutsBusy() const {
    return savedLayoutsBusy_;
}

QString RuntimeClient::savedLayoutsStatus() const {
    if (!savedLayoutsSupported()) {
        return QStringLiteral("NotSupported");
    }
    return savedLayouts_.status;
}

QString RuntimeClient::savedLayoutsDiagnostic() const {
    return savedLayouts_.diagnostic;
}

QString RuntimeClient::savedLayoutsDeviceIdentity() const {
    return savedLayouts_.deviceIdentity;
}

bool RuntimeClient::hasRuntimeCapability(
    const QString &capability) const {
    return capabilitiesReady_ &&
           runtimeCapabilities_.contains(capability);
}

bool RuntimeClient::hasDeviceCapability(
    const QString &capability) const {
    return deviceCapabilitiesReady_ &&
           deviceCapabilities_.contains(capability);
}

bool RuntimeClient::requestSupportSnapshot() {
    if (supportSnapshotBusy_ || !supportSnapshotAvailable() ||
        !handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        return false;
    }

    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    const quint64 requestAttempt = ++supportSnapshotAttempt_;
    supportSnapshotBusy_ = true;
    emit supportSnapshotStateChanged();

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetSupportSnapshotV1")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         requestAttempt]() {
            const QDBusPendingReply<QString> reply = *watcher;
            watcher->deleteLater();
            if (requestAttempt != supportSnapshotAttempt_ ||
                !supportSnapshotBusy_ ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }

            supportSnapshotBusy_ = false;
            emit supportSnapshotStateChanged();
            if (!reply.isValid()) {
                setDiagnostic(reply.error().message());
                emit supportSnapshotFailed(tr(
                    "Could not collect the runtime support snapshot"));
                return;
            }

            QString validationError;
            if (!tryx::supportSnapshotV1IsValid(
                    reply.value(), &validationError)) {
                setDiagnostic(validationError);
                emit supportSnapshotFailed(tr(
                    "The runtime returned an invalid support snapshot"));
                return;
            }
            const QJsonDocument canonical = QJsonDocument::fromJson(
                reply.value().toUtf8());
            emit supportSnapshotReady(QString::fromUtf8(
                canonical.toJson(QJsonDocument::Compact)));
        });
    return true;
}

bool RuntimeClient::requestDeviceMediaMetadata(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    if (!capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceMediaMetadataV1Token()) ||
        !deviceMediaArtifactMetadataIdentityIsValid(artifact)) {
        return false;
    }
    if (offline_) {
        ++deviceMediaMetadataAttempt_;
        deviceMediaMetadataPending_ = true;
        pendingDeviceMediaMetadataArtifactId_ = artifact.artifactId;
        offlineRequests_.append({
            QStringLiteral("GetDeviceMediaMetadataV1"),
            {artifact.artifactId, artifact.leaseId},
            artifact.operationId,
            QStringLiteral("GetDeviceMediaMetadataV1"),
        });
        return true;
    }
    if (!handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        return false;
    }

    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    const quint64 requestAttempt = ++deviceMediaMetadataAttempt_;
    deviceMediaMetadataPending_ = true;
    pendingDeviceMediaMetadataArtifactId_ = artifact.artifactId;

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetDeviceMediaMetadataV1"),
            artifact.artifactId, artifact.leaseId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         requestAttempt, artifact]() {
            const QDBusPendingReply<
                TryxRuntimeDeviceMediaMetadataV1> reply = *watcher;
            watcher->deleteLater();
            if (requestAttempt != deviceMediaMetadataAttempt_ ||
                !deviceMediaMetadataPending_ ||
                pendingDeviceMediaMetadataArtifactId_ !=
                    artifact.artifactId ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }

            deviceMediaMetadataPending_ = false;
            pendingDeviceMediaMetadataArtifactId_.clear();
            if (!reply.isValid()) {
                const QString error =
                    reply.error().message().trimmed().isEmpty()
                    ? tr("Could not read the device media metadata")
                    : reply.error().message();
                setDiagnostic(error);
                emit deviceMediaMetadataFailed(
                    artifact.artifactId, error);
                return;
            }

            const TryxRuntimeDeviceMediaMetadataV1 metadata =
                reply.value();
            if (!tryxRuntimeDeviceMediaMetadataV1IsValid(metadata) ||
                !deviceMediaMetadataMatchesArtifact(
                    metadata, artifact)) {
                const QString error = tr(
                    "The runtime returned invalid device media metadata");
                setDiagnostic(error);
                emit deviceMediaMetadataFailed(
                    artifact.artifactId, error);
                return;
            }
            emit deviceMediaMetadataReady(metadata);
        });
    return true;
}

QString RuntimeClient::formatTemperature(
    bool available, double celsius) const {
    if (!available) {
        return QStringLiteral("\u2014");
    }
    const QString formatted = tryxFormatTemperature(
        celsius, presentationPreferences_.temperatureUnit);
    return formatted.isEmpty() ? QStringLiteral("\u2014")
                               : formatted;
}

void RuntimeClient::setPresentationPreferences(
    const QString &temperatureUnit,
    const QString &timeFormat) {
    const auto reject = [this](const QString &message) {
        setDiagnostic(message);
        emit userMessage(message, true);
    };
    if (!tryxTemperatureUnitIsValid(temperatureUnit) ||
        !tryxTimeFormatIsValid(timeFormat)) {
        reject(tr("The presentation preferences are invalid"));
        return;
    }
    if (!presentationPreferencesReady_ ||
        !capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimePresentationPreferencesV1Token()) ||
        !handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        reject(tr("Presentation preferences are unavailable for the active runtime"));
        return;
    }
    if (presentationPreferencesBusy_) {
        reject(tr("Presentation preferences are already being saved"));
        return;
    }
    if (temperatureUnit == presentationPreferences_.temperatureUnit &&
        timeFormat == presentationPreferences_.timeFormat) {
        return;
    }

    presentationPreferencesBusy_ = true;
    const quint64 mutationAttempt =
        ++presentationPreferencesMutationAttempt_;
    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    const quint64 expectedRevision =
        presentationPreferences_.revision;
    emit presentationPreferencesChanged();

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("SetPresentationPreferencesV1"),
            QVariant::fromValue(expectedRevision),
            temperatureUnit, timeFormat),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         mutationAttempt, temperatureUnit, timeFormat]() {
            const QDBusPendingReply<
                TryxRuntimePresentationPreferencesV1> reply = *watcher;
            watcher->deleteLater();
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner) ||
                mutationAttempt !=
                    presentationPreferencesMutationAttempt_) {
                return;
            }

            if (!reply.isValid()) {
                const QString message = reply.error().message();
                setDiagnostic(message);
                emit userMessage(message, true);
                requestPresentationPreferences(
                    epoch, handshakeAttempt, owner, true);
                return;
            }

            const TryxRuntimePresentationPreferencesV1 confirmed =
                reply.value();
            if (confirmed.temperatureUnit != temperatureUnit ||
                confirmed.timeFormat != timeFormat ||
                !tryxPresentationPreferencesAreValid(confirmed)) {
                const QString message = tr(
                    "The runtime returned invalid presentation preferences after saving");
                setDiagnostic(message);
                emit userMessage(message, true);
                requestPresentationPreferences(
                    epoch, handshakeAttempt, owner, true);
                return;
            }
            presentationPreferencesBusy_ = false;
            applyPresentationPreferencesSnapshot(confirmed);
            emit presentationPreferencesChanged();
        });
}

void RuntimeClient::refreshSavedLayouts() {
    if (!savedLayoutsSupported() || savedLayoutsBusy_) {
        return;
    }
    if (offline_) {
        savedLayoutsBusy_ = true;
        ++savedLayoutsAttempt_;
        const QString method = savedLayoutsV2Supported() ? QStringLiteral("GetSavedLayoutsV2") : QStringLiteral("GetSavedLayoutsV1");
        offlineRequests_.append({
            method, {}, {}, method,
        });
        emit savedLayoutsChanged();
        return;
    }
    requestSavedLayouts(
        serviceEpoch_, handshakeAttempt_, runtimeOwner_);
}

QVariantMap RuntimeClient::savedLayoutDraft(
    const QString &layoutId) const {
    TryxRuntimeSavedLayoutV2 layout;
    if (!savedLayoutsReady() ||
        !savedLayoutModel_.layoutV2ById(layoutId, &layout) ||
        !savedLayoutIsCanonical(
            layout, savedLayouts_.deviceIdentity,
            savedLayouts_.productId)) {
        return {};
    }

    const TryxRuntimeApplyRequest &request = layout.request;
    const bool split = request.screenMode ==
        QStringLiteral("Screen Splitting");
    QVariantMap layoutDraft;
    layoutDraft.insert(QStringLiteral("split"), split);
    layoutDraft.insert(QStringLiteral("media"), request.media);
    layoutDraft.insert(QStringLiteral("playMode"), request.playMode);
    if (savedLayoutsV2Supported()) layoutDraft.insert(QStringLiteral("badgeChoices"), tryxOverlayBadgesV1ToJson(layout.badges).toVariantMap());
    if (split) {
        layoutDraft.insert(
            QStringLiteral("leftMetrics"), request.sysinfoLabels);
        layoutDraft.insert(
            QStringLiteral("rightMetrics"), request.sysinfoLabels2);
        layoutDraft.insert(
            QStringLiteral("leftBadges"), request.settingsBadges);
        layoutDraft.insert(
            QStringLiteral("rightBadges"), request.settingsBadges2);
        layoutDraft.insert(
            QStringLiteral("leftPosition"), request.settingsPosition);
        layoutDraft.insert(
            QStringLiteral("leftColor"), request.settingsColor);
        layoutDraft.insert(
            QStringLiteral("leftAlignment"), request.settingsAlign);
        layoutDraft.insert(
            QStringLiteral("rightPosition"), request.settingsPosition2);
        layoutDraft.insert(
            QStringLiteral("rightColor"), request.settingsColor2);
        layoutDraft.insert(
            QStringLiteral("rightAlignment"), request.settingsAlign2);
    } else {
        layoutDraft.insert(
            QStringLiteral("metrics"), request.sysinfoLabels);
        layoutDraft.insert(
            QStringLiteral("badges"), request.settingsBadges);
        layoutDraft.insert(
            QStringLiteral("position"), request.settingsPosition);
        layoutDraft.insert(
            QStringLiteral("color"), request.settingsColor);
        layoutDraft.insert(
            QStringLiteral("alignment"), request.settingsAlign);
    }

    QVariantMap orientation;
    orientation.insert(
        QStringLiteral("mirror"), request.display.mirrorMode);
    orientation.insert(
        QStringLiteral("waterfall"), request.display.waterfallMode);
    return {
        {QStringLiteral("layoutId"), layout.layoutId},
        {QStringLiteral("revision"), QString::number(layout.revision)},
        {QStringLiteral("deviceIdentity"), layout.deviceIdentity},
        {QStringLiteral("name"), layout.name},
        {QStringLiteral("layout"), layoutDraft},
        {QStringLiteral("brightness"), request.display.brightness},
        {QStringLiteral("orientation"), orientation},
    };
}

QString RuntimeClient::savedLayoutIdForName(
    const QString &name) const {
    return savedLayoutsReady()
        ? savedLayoutModel_.idForName(name)
        : QString();
}

void RuntimeClient::putSavedLayout(
    const QString &name, const QString &overwriteLayoutId,
    const QVariantMap &fullDraft) {
    const QString requestedId = overwriteLayoutId.trimmed();
    const auto reject =
        [this, &requestedId](const QString &message) {
            setDiagnostic(message);
            emit savedLayoutPutFinished(
                requestedId, {}, {}, false, message);
            emit userMessage(message, true);
        };
    if (!savedLayoutsReady() || savedLayoutsBusy_ ||
        operationBusy()) {
        reject(tr(
            "Saved layouts are unavailable while the display state or another request is unresolved"));
        return;
    }

    TryxRuntimeSavedLayoutV2 layout;
    QString validationError;
    if (!savedLayoutFromDraft(
            name, requestedId, fullDraft,
            &layout, &validationError)) {
        reject(validationError.isEmpty()
                   ? tr("The saved layout draft is invalid")
                   : validationError);
        return;
    }

    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!offline_ && !handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        const QString message = tr(
            "The runtime changed before the saved layout was stored");
        setSavedLayoutsUnavailable(message);
        reject(message);
        return;
    }
    const bool version2 = savedLayoutsV2Supported();
    const QString method = version2 ? QStringLiteral("PutSavedLayoutV2") : QStringLiteral("PutSavedLayoutV1");
    TryxRuntimeSavedLayoutV1 legacyLayout;
    if (!version2 && !tryxSavedLayoutV2ToV1(layout, &legacyLayout)) {
        reject(tr("Custom badge text cannot be stored through the legacy saved-layout API"));
        return;
    }
    const quint64 expectedRevision = savedLayouts_.revision;
    const TryxRuntimeSavedLayoutsSnapshotV2 expectedSnapshot =
        savedLayouts_;
    const quint64 requestAttempt = ++savedLayoutsAttempt_;
    savedLayoutsBusy_ = true;
    emit savedLayoutsChanged();

    const QVariantList arguments = {
        QVariant::fromValue(expectedRevision),
        version2 ? QVariant::fromValue(layout) : QVariant::fromValue(legacyLayout),
    };
    if (offline_) {
        offlineRequests_.append({
            method, arguments, {}, method,
        });
        return;
    }

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCallWithArgumentList(
            method, arguments),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         requestAttempt, requestedId, layout,
         expectedSnapshot, version2]() {
            const auto reply = savedLayoutsReply(watcher, version2);
            watcher->deleteLater();
            if (requestAttempt != savedLayoutsAttempt_ ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }
            savedLayoutsBusy_ = false;
            if (!reply.isValid()) {
                const QString message = reply.error().message().isEmpty()
                    ? tr("The saved layout could not be stored")
                    : reply.error().message();
                const SavedLayoutMutationFailureAction action =
                    savedLayoutMutationFailureAction(reply.error());
                if (action !=
                    SavedLayoutMutationFailureAction::KeepConfirmed) {
                    setSavedLayoutsUnavailable(message);
                } else {
                    emit savedLayoutsChanged();
                }
                setDiagnostic(message);
                emit savedLayoutPutFinished(
                    requestedId, {}, {}, false, message);
                emit userMessage(message, true);
                if (action ==
                    SavedLayoutMutationFailureAction::Reconcile) {
                    QTimer::singleShot(
                        0, this, &RuntimeClient::refreshSavedLayouts);
                }
                return;
            }

            const TryxRuntimeSavedLayoutsSnapshotV2 snapshot =
                reply.value();
            TryxRuntimeSavedLayoutV2 confirmed;
            if (!savedLayoutsSnapshotIsValid(snapshot) ||
                snapshot.deviceIdentity !=
                    savedLayoutConnectionIdentity() ||
                snapshot.productId != connection_.productId ||
                !savedLayoutPutReplyIsExact(
                    expectedSnapshot, layout, snapshot, &confirmed)) {
                const QString message = tr(
                    "The runtime did not confirm the stored layout");
                setSavedLayoutsUnavailable(message);
                setDiagnostic(message);
                emit savedLayoutPutFinished(
                    requestedId, {}, {}, false, message);
                emit userMessage(message, true);
                return;
            }
            QString snapshotError;
            if (!applySavedLayoutsSnapshot(
                    snapshot, &snapshotError)) {
                savedLayoutsBusy_ = false;
                setDiagnostic(snapshotError);
                emit savedLayoutPutFinished(
                    requestedId, {}, {}, false, snapshotError);
                emit userMessage(snapshotError, true);
                return;
            }
            setDiagnostic({});
            emit savedLayoutPutFinished(
                requestedId, confirmed.layoutId,
                QString::number(confirmed.revision), true, {});
            emit userMessage(tr("Saved layout stored"), false);
        });
}

void RuntimeClient::deleteSavedLayout(const QString &layoutId) {
    const QString requestedId = layoutId.trimmed();
    const auto reject =
        [this, &requestedId](const QString &message) {
            setDiagnostic(message);
            emit savedLayoutDeleteFinished(
                requestedId, false, message);
            emit userMessage(message, true);
        };
    TryxRuntimeSavedLayoutV2 existing;
    if (!savedLayoutsReady() || savedLayoutsBusy_ ||
        operationBusy() ||
        !savedLayoutModel_.layoutV2ById(requestedId, &existing)) {
        reject(tr("The saved layout is unavailable for deletion"));
        return;
    }

    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!offline_ && !handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        const QString message = tr(
            "The runtime changed before the saved layout was deleted");
        setSavedLayoutsUnavailable(message);
        reject(message);
        return;
    }
    const bool version2 = savedLayoutsV2Supported();
    const QString method = version2 ? QStringLiteral("DeleteSavedLayoutV2") : QStringLiteral("DeleteSavedLayoutV1");
    if (!version2 && tryxOverlayBadgesHaveCustomText(existing.badges)) {
        reject(tr("Custom badge layouts require the versioned saved-layout API"));
        return;
    }
    const quint64 expectedRevision = savedLayouts_.revision;
    const TryxRuntimeSavedLayoutsSnapshotV2 expectedSnapshot =
        savedLayouts_;
    const quint64 requestAttempt = ++savedLayoutsAttempt_;
    savedLayoutsBusy_ = true;
    emit savedLayoutsChanged();
    const QVariantList arguments = {
        QVariant::fromValue(expectedRevision), requestedId};
    if (offline_) {
        offlineRequests_.append({
            method, arguments, {}, method,
        });
        return;
    }

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCallWithArgumentList(
            method, arguments),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         requestAttempt, requestedId, expectedSnapshot, version2]() {
            const auto reply = savedLayoutsReply(watcher, version2);
            watcher->deleteLater();
            if (requestAttempt != savedLayoutsAttempt_ ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }
            savedLayoutsBusy_ = false;
            if (!reply.isValid()) {
                const QString message = reply.error().message().isEmpty()
                    ? tr("The saved layout could not be deleted")
                    : reply.error().message();
                const SavedLayoutMutationFailureAction action =
                    savedLayoutMutationFailureAction(reply.error());
                if (action !=
                    SavedLayoutMutationFailureAction::KeepConfirmed) {
                    setSavedLayoutsUnavailable(message);
                } else {
                    emit savedLayoutsChanged();
                }
                setDiagnostic(message);
                emit savedLayoutDeleteFinished(
                    requestedId, false, message);
                emit userMessage(message, true);
                if (action ==
                    SavedLayoutMutationFailureAction::Reconcile) {
                    QTimer::singleShot(
                        0, this, &RuntimeClient::refreshSavedLayouts);
                }
                return;
            }
            const TryxRuntimeSavedLayoutsSnapshotV2 snapshot =
                reply.value();
            if (!savedLayoutsSnapshotIsValid(snapshot) ||
                snapshot.deviceIdentity !=
                    savedLayoutConnectionIdentity() ||
                snapshot.productId != connection_.productId ||
                !savedLayoutDeleteReplyIsExact(
                    expectedSnapshot, requestedId, snapshot)) {
                const QString message = tr(
                    "The runtime did not confirm the layout deletion");
                setSavedLayoutsUnavailable(message);
                setDiagnostic(message);
                emit savedLayoutDeleteFinished(
                    requestedId, false, message);
                emit userMessage(message, true);
                return;
            }
            QString snapshotError;
            if (!applySavedLayoutsSnapshot(
                    snapshot, &snapshotError) ||
                savedLayoutModel_.layoutV2ById(requestedId, static_cast<TryxRuntimeSavedLayoutV2 *>(nullptr))) {
                const QString message = snapshotError.isEmpty()
                    ? tr("The runtime did not confirm the layout deletion")
                    : snapshotError;
                setDiagnostic(message);
                emit savedLayoutDeleteFinished(
                    requestedId, false, message);
                emit userMessage(message, true);
                return;
            }
            setDiagnostic({});
            emit savedLayoutDeleteFinished(
                requestedId, true, {});
            emit userMessage(tr("Saved layout deleted"), false);
        });
}

QString RuntimeClient::submitSavedLayoutDraft(
    const QString &layoutId, const QString &revisionDecimal,
    const QVariantMap &fullDraft) {
    if (!mutationReady(tr("Apply saved layout")) ||
        !savedLayoutsReady() || savedLayoutsBusy_) {
        const QString message = tr(
            "The saved layout cannot be applied until its current device snapshot is ready");
        setDiagnostic(message);
        emit userMessage(message, true);
        return {};
    }
    TryxRuntimeSavedLayoutV2 layout;
    quint64 expectedRevision = 0;
    if (!savedLayoutModel_.layoutV2ById(layoutId, &layout) ||
        !parseSavedLayoutRevision(
            revisionDecimal, &expectedRevision) ||
        expectedRevision != layout.revision) {
        const QString message = tr(
            "The saved layout changed; load it again before applying");
        setDiagnostic(message);
        emit userMessage(message, true);
        return {};
    }
    TryxRuntimeApplyRequest request;
    QString validationError;
    if (!fullSavedLayoutDraftToRequest(
            fullDraft, &request, &validationError)) {
        setDiagnostic(validationError);
        emit userMessage(validationError, true);
        return {};
    }
    return beginDisplaySubmission(
        request, true, true, true, false, false,
        QStringLiteral("QueueSavedLayoutApplyV1"),
        layout.layoutId, layout.revision,
        fullDraft.value(QStringLiteral("layout")).toMap().value(QStringLiteral("badgeChoices")).toMap());
}

MediaCatalogModel *RuntimeClient::mediaModel() {
    return &mediaModel_;
}

OperationListModel *RuntimeClient::operationModel() {
    return &operationModel_;
}

bool RuntimeClient::operationBusy() const {
    return !activeOperationId_.isEmpty() ||
           legacyUpload_.active() ||
           displaySubmission_.active();
}

QString RuntimeClient::activeOperationId() const {
    if (legacyUpload_.active()) {
        return legacyUpload_.operationId;
    }
    if (displaySubmission_.active()) {
        return displaySubmission_.id;
    }
    return activeOperationId_;
}

QString RuntimeClient::operationSummary() const {
    if (legacyUpload_.active()) {
        return legacyUpload_.rejectionEmitted
            ? tr("Legacy upload timed out; waiting for the device worker to release the protected source")
            : tr("Uploading media to the legacy device");
    }
    if (displaySubmission_.active() &&
        displaySubmission_.unresolved) {
        return tr("The display apply outcome is unknown; review or discard the local draft before continuing");
    }
    if (displaySubmission_.active() &&
        activeOperation_.id.isEmpty()) {
        return tr("Applying display changes");
    }
    if (activeOperation_.id.isEmpty()) {
        return {};
    }
    QString title = activeOperation_.subject.trimmed();
    if (title.isEmpty()) {
        title = activeOperation_.kind;
    }
    QString detail = activeOperation_.message.trimmed();
    if (detail.isEmpty()) {
        detail = activeOperation_.stage;
    }
    return detail.isEmpty() ? title
                            : tr("%1: %2").arg(title, detail);
}

double RuntimeClient::operationProgress() const {
    if (legacyUpload_.active()) {
        return 0.0;
    }
    if (activeOperation_.total <= 0) {
        return 0.0;
    }
    return qBound(
        0.0,
        static_cast<double>(activeOperation_.completed) /
            static_cast<double>(activeOperation_.total),
        1.0);
}

QStringList RuntimeClient::availableMetrics() const {
    return metrics_.availableMetrics;
}

bool RuntimeClient::metricsCatalogReady() const {
    return metricsCatalogReady_;
}

QStringList RuntimeClient::metricsCatalog() const {
    return metricsCatalog_;
}

bool RuntimeClient::metricsEnabled() const {
    return metrics_.enabled;
}

bool RuntimeClient::samplingActive() const {
    return metrics_.samplingActive;
}

QStringList RuntimeClient::activeMetrics() const {
    return metrics_.metrics;
}

QString RuntimeClient::metricsAlignment() const {
    return metrics_.alignment;
}

QString RuntimeClient::metricsColor() const {
    return QStringLiteral("#%1")
        .arg(metrics_.textColor & 0x00ffffff, 6, 16,
             QLatin1Char('0'));
}

bool RuntimeClient::displayStateValid() const {
    if (legacyConnected()) {
        const QString identity = legacyDisplayIdentity();
        return display_.valid && legacyLayoutConfirmed_ &&
               !identity.isEmpty() &&
               legacyDisplayStateIdentity_ == identity;
    }
    if (!capabilitiesReady_) return false;
    if (usesDisplaySnapshotV1()) return !displaySnapshotReadFailed_ && displaySnapshot_.status == QStringLiteral("HostAccepted")
        && displaySnapshotContextIsCurrent(displaySnapshot_);
    return connection_.printerClassDevicePresent &&
           display_.valid;
}

bool RuntimeClient::legacyDisplayLayoutConfirmed() const {
    const QString identity = legacyDisplayIdentity();
    return legacyConnected() && legacyLayoutConfirmed_ &&
           !identity.isEmpty() &&
           legacyDisplayStateIdentity_ == identity;
}

bool RuntimeClient::legacyDisplayBrightnessConfirmed() const {
    const QString identity = legacyDisplayIdentity();
    return legacyConnected() && legacyBrightnessConfirmed_ &&
           !identity.isEmpty() &&
           legacyDisplayStateIdentity_ == identity;
}

quint64 RuntimeClient::displayRevision() const {
    return usesDisplaySnapshotV1() ? displaySnapshot_.revision : display_.revision;
}

bool RuntimeClient::usesDisplaySnapshotV1() const {
    return !legacyConnected() && (displaySnapshotRequired_
        || (capabilitiesReady_ && runtimeCapabilities_.contains(tryxRuntimeDisplaySnapshotV1Token())));
}

bool RuntimeClient::customBadgeTextSupported() const {
    return serviceAvailable_ && compatible_ && capabilitiesReady_ && deviceCapabilitiesReady_
        && connection_.printerClassConnected && connection_.productId == QStringLiteral("391a:1021")
        && runtimeCapabilities_.contains(tryxRuntimeApplyWithBadgesV1Token())
        && runtimeCapabilities_.contains(tryxRuntimeDisplaySnapshotV1Token())
        && runtimeCapabilities_.contains(tryxRuntimeSavedLayoutsV2Token())
        && deviceCapabilities_.contains(tryxDeviceOverlayBadgeTextV1Token())
        && deviceCapabilitiesSnapshot_.deviceIdentity == connection_.serial.trimmed()
        && !deviceCapabilitiesSnapshot_.deviceIdentity.isEmpty()
        && deviceCapabilitiesSnapshot_.connectionRevision == connection_.revision
        && deviceCapabilitiesSnapshot_.physicalGeneration != 0;
}

QVariantMap RuntimeClient::displayBadgeChoices() const {
    return tryxOverlayBadgesV1ToJson(displaySnapshot_.status == QStringLiteral("HostAccepted")
        ? displaySnapshot_.badges : TryxRuntimeOverlayBadgesV1{}).toVariantMap();
}

QString RuntimeClient::badgeTextError(const QString &mode, const QString &text) const {
    return tryxNormalizeBadgeTextV1({mode, text}, nullptr) ? QString()
        : tr("Enter one line of 1-32 characters (up to 128 UTF-8 bytes), without control or formatting characters");
}

bool RuntimeClient::normalizeBadgeDraft(const QVariantMap &draft, const TryxRuntimeApplyRequest &request,
    TryxRuntimeOverlayBadgesV1 *badges) const {
    TryxRuntimeOverlayBadgesV1 parsed;
    if (!draft.isEmpty()) {
        QJsonObject json = QJsonObject::fromVariantMap(draft);
        for (const auto &key : {QStringLiteral("primaryCpu"), QStringLiteral("primaryGpu"),
                               QStringLiteral("secondaryCpu"), QStringLiteral("secondaryGpu")}) {
            if (!json.value(key).isObject()) return false;
            auto slot = json.value(key).toObject();
            if (slot.size() != 2 || !slot.value(QStringLiteral("mode")).isString() || !slot.value(QStringLiteral("text")).isString()) return false;
            TryxRuntimeBadgeTextV1 normalized;
            if (!tryxNormalizeBadgeTextV1({slot.value(QStringLiteral("mode")).toString(), slot.value(QStringLiteral("text")).toString()}, &normalized)) return false;
            slot.insert(QStringLiteral("text"), normalized.text);
            json.insert(key, slot);
        }
        if (!tryxOverlayBadgesV1FromJson(json, &parsed)) return false;
    }
    return tryxNormalizeOverlayBadgesV1(parsed, request.settingsBadges, request.settingsBadges2,
        request.screenMode == QStringLiteral("Screen Splitting"), badges);
}

bool RuntimeClient::displaySnapshotContextIsCurrent(const TryxRuntimeDisplaySnapshotV1 &snapshot) const {
    return usesDisplaySnapshotV1() && serviceAvailable_ && compatible_ && capabilitiesReady_ && deviceCapabilitiesReady_
        && runtimeCapabilities_.contains(tryxRuntimeDisplaySnapshotV1Token())
        && connection_.printerClassConnected && connection_.printerClassDevicePresent && connection_.displaySessionActive
        && !connection_.serial.isEmpty() && snapshot.connectionRevision == connection_.revision
        && snapshot.productId == connection_.productId && snapshot.physicalGeneration != 0
        && snapshot.physicalGeneration == deviceCapabilitiesSnapshot_.physicalGeneration
        && deviceCapabilitiesSnapshot_.deviceIdentity == connection_.serial.trimmed()
        && deviceCapabilitiesSnapshot_.connectionRevision == connection_.revision
        && (snapshot.status != QStringLiteral("HostAccepted") || snapshot.display.deviceSerial == connection_.serial.trimmed());
}

bool RuntimeClient::displaySubmissionContextIsCurrent() const {
    return displaySubmission_.coherentSnapshot && displayStateValid()
        && (!tryxOverlayBadgesHaveCustomText(displaySubmission_.expectedBadges) || customBadgeTextSupported())
        && displaySubmission_.serviceEpoch == serviceEpoch_ && displaySubmission_.handshakeAttempt == handshakeAttempt_
        && displaySubmission_.owner == runtimeOwner_ && displaySubmission_.productId == connection_.productId
        && displaySubmission_.physicalGeneration == displaySnapshot_.physicalGeneration
        && displaySubmission_.deviceIdentity == connection_.serial.trimmed();
}

bool RuntimeClient::applyDisplaySnapshotV1(const TryxRuntimeDisplaySnapshotV1 &snapshot) {
    if (!tryxDisplaySnapshotV1IsValid(snapshot) || !displaySnapshotContextIsCurrent(snapshot)
        || snapshot.revision < displaySnapshot_.revision) return false;
    // A connection revision can advance independently of the display bundle.
    if (snapshot.revision == displaySnapshot_.revision && snapshot == displaySnapshot_ && !displaySnapshotReadFailed_) return true;
    if (snapshot.revision == displaySnapshot_.revision) {
        auto current = displaySnapshot_;
        current.connectionRevision = snapshot.connectionRevision;
        if (!(current == snapshot)) return false;
    }
    const QString oldIdentity = displayDeviceIdentity();
    displaySnapshotRequired_ = true;
    displaySnapshotReadFailed_ = false;
    displaySnapshot_ = snapshot;
    if (snapshot.status == QStringLiteral("HostAccepted")) {
        display_ = snapshot.display;
        displayRevisionReceived_ = true;
        legacyDisplayStateIdentity_.clear();
        legacyLayoutConfirmed_ = legacyBrightnessConfirmed_ = false;
        completeConnectionRevisionReconciliation();
    }
    // Pending/unavailable never replaces a user's baseline with empty Auto.
    emit displayChanged();
    if (oldIdentity != displayDeviceIdentity()) emit displayDeviceIdentityChanged();
    if (!displaySubmission_.active() || displaySubmission_.resultEmitted || !displaySubmission_.coherentSnapshot
        || snapshot.revision <= displaySubmission_.startingDisplayRevision) return true;
    displaySubmission_.matchingStateObserved = false;
    if (snapshot.status != QStringLiteral("HostAccepted") || snapshot.acceptedOperationId != displaySubmission_.id
        || !displaySubmissionContextIsCurrent()) return true;
    if (snapshot.badges != displaySubmission_.expectedBadges || !displaySubmissionMatches(snapshot.display)) {
        finishDisplaySubmission(QStringLiteral("Unresolved"), tr("The confirmed display state does not match the submitted draft"), true);
        return true;
    }
    displaySubmission_.matchingStateObserved = true;
    if (displaySubmission_.terminalSucceeded)
        finishDisplaySubmission(QStringLiteral("Succeeded"), tr("The display changes were confirmed"));
    return true;
}

void RuntimeClient::refreshDisplaySnapshotV1() {
    if (!usesDisplaySnapshotV1() || !capabilitiesReady_ || !deviceCapabilitiesReady_
        || !runtimeCapabilities_.contains(tryxRuntimeDisplaySnapshotV1Token())
        || !handshakeContextIsCurrent(serviceEpoch_, handshakeAttempt_, runtimeOwner_)) return;
    if (displaySnapshotReadPending_) {
        displaySnapshotReadAgain_ = true;
        return;
    }
    const quint64 epoch = serviceEpoch_, handshake = handshakeAttempt_, attempt = ++displaySnapshotReadAttempt_;
    const quint64 connectionRevision = connection_.revision;
    const QString owner = runtimeOwner_;
    displaySnapshotReadPending_ = true;
    QDBusInterface runtime(owner, tryxRuntimeObjectPath(), tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(runtime.asyncCall(QStringLiteral("GetDisplaySnapshotV1")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch, handshake, attempt, owner, connectionRevision]() {
        const QDBusPendingReply<TryxRuntimeDisplaySnapshotV1> reply = *watcher;
        watcher->deleteLater();
        if (attempt != displaySnapshotReadAttempt_) return;
        displaySnapshotReadPending_ = false;
        if (!handshakeContextIsCurrent(epoch, handshake, owner)) return;
        if (connectionRevision == connection_.revision && (!reply.isValid() || !applyDisplaySnapshotV1(reply.value()))) {
            displaySnapshotReadFailed_ = true;
            setDiagnostic(tr("The runtime did not return a coherent display snapshot for the current device"));
            emit displayChanged();
            if (reply.isValid()) {
                const auto snapshot = reply.value();
                if (tryxDisplaySnapshotV1IsValid(snapshot)
                    && snapshot.productId == connection_.productId
                    && snapshot.physicalGeneration == deviceCapabilitiesSnapshot_.physicalGeneration
                    && (snapshot.status != QStringLiteral("HostAccepted")
                        || snapshot.display.deviceSerial == connection_.serial.trimmed())) {
                    reconcileConnectionRevision(snapshot.connectionRevision);
                }
            }
        }
        const bool again = displaySnapshotReadAgain_;
        displaySnapshotReadAgain_ = false;
        if (again) refreshDisplaySnapshotV1();
    });
}

QString RuntimeClient::displayDeviceIdentity() const {
    return legacyConnected()
        ? legacyDisplayIdentity()
        : display_.deviceSerial.trimmed();
}

int RuntimeClient::brightness() const {
    return display_.brightness;
}

bool RuntimeClient::backlightEnabled() const {
    return display_.backlightEnabled;
}

bool RuntimeClient::mirrorMode() const {
    return display_.mirrorMode;
}

bool RuntimeClient::waterfallMode() const {
    return display_.waterfallMode;
}

QString RuntimeClient::currentScreenMode() const {
    return display_.screenMode;
}

QString RuntimeClient::currentPlayMode() const {
    return display_.playMode;
}

QStringList RuntimeClient::displayedMedia() const {
    return display_.media;
}

QStringList RuntimeClient::displayLeftMetrics() const {
    return display_.sysinfoLabels;
}

QStringList RuntimeClient::displayRightMetrics() const {
    return display_.sysinfoLabels2;
}

QStringList RuntimeClient::displayLeftBadges() const {
    return display_.settingsBadges;
}

QStringList RuntimeClient::displayRightBadges() const {
    return display_.settingsBadges2;
}

QString RuntimeClient::displayLeftPosition() const {
    return display_.settingsPosition.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition;
}

QString RuntimeClient::displayLeftColor() const {
    return display_.settingsColor.isEmpty()
        ? QStringLiteral("#dcdcdc")
        : display_.settingsColor;
}

QString RuntimeClient::displayLeftAlignment() const {
    return display_.settingsAlign.isEmpty()
        ? QStringLiteral("Left")
        : display_.settingsAlign;
}

QString RuntimeClient::displayRightPosition() const {
    return display_.settingsPosition2.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition2;
}

QString RuntimeClient::displayRightColor() const {
    return display_.settingsColor2.isEmpty()
        ? QStringLiteral("#dcdcdc")
        : display_.settingsColor2;
}

QString RuntimeClient::displayRightAlignment() const {
    return display_.settingsAlign2.isEmpty()
        ? QStringLiteral("Right")
        : display_.settingsAlign2;
}

QString RuntimeClient::queueUploadWithTransform(
    const QString &localPath,
    const TryxRuntimeMediaTransform &transform) {
    return queueUploadWithPreparationProfile(
        localPath,
        tryxFullFrameMediaPreparationProfile(transform));
}

QString RuntimeClient::queueUploadWithPreparationProfile(
    const QString &localPath,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (!mutationReady(tr("Upload"))) {
        return {};
    }
    if (localPath.trimmed().isEmpty()) {
        setDiagnostic(tr("The upload source path is empty"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    QString profileError;
    if (!tryxMediaPreparationProfileV1IsValid(
            profile, &profileError)) {
        setDiagnostic(tr("Media preparation profile is invalid: %1")
                          .arg(profileError));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const bool split =
        profile.target == QStringLiteral("SplitArea");
    const bool profileContract = hasRuntimeCapability(
        tryxRuntimeMediaPreparationProfileV1Token());
    if (split &&
        (!profileContract ||
         !hasDeviceCapability(
             tryxDeviceDisplayConfigurationV1Token()) ||
         !hasDeviceCapability(
             tryxDeviceMediaSplitAreaV1Token()))) {
        setDiagnostic(tr(
            "Split-area media preparation is not supported by the connected runtime and device"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    if (legacyConnected()) {
        if (split ||
            !tryxMediaTransformIsLegacyFit(profile.transform)) {
            setDiagnostic(tr(
                "Legacy serial/ADB upload supports only the default Fit transform. Reset sizing, rotation, zoom, position and background before uploading."));
            emit userMessage(diagnostic_, true);
            return {};
        }
        QString claimError;
        if (!claimLegacyUploadSource(
                operationId, localPath, &claimError)) {
            setDiagnostic(claimError);
            emit userMessage(diagnostic_, true);
            return {};
        }
        beginLegacyUpload(operationId);
        return operationId;
    }
    if (profileContract) {
        sendOperation(
            QStringLiteral("QueueUploadWithPreparationProfileV1"),
            {operationId, localPath,
             QVariant::fromValue(profile)},
            operationId, QStringLiteral("Upload"));
        return operationId;
    }
    sendOperation(
        QStringLiteral("QueueUploadWithTransform"),
        {operationId, localPath,
         QVariant::fromValue(profile.transform)},
        operationId, QStringLiteral("Upload"));
    return operationId;
}

QString RuntimeClient::queueStageDeviceMedia(
    const QString &mediaId) {
    if (!mutationReady(tr("Export or edit"))) {
        return {};
    }
    if (mediaId.trimmed().isEmpty() ||
        !mediaModel_.canStageDeviceCopy(mediaId)) {
        const QString reason =
            mediaModel_.deviceCopyBlockReason(mediaId);
        setDiagnostic(
            reason.isEmpty()
                ? tr("The selected media cannot be staged")
                : reason);
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    sendOperation(
        QStringLiteral("QueueStageDeviceMedia"),
        {operationId, mediaId}, operationId,
        QStringLiteral("StageDeviceMedia"));
    return operationId;
}

void RuntimeClient::claimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId) {
    if (!serviceAvailable_ || !compatible_ ||
        operationId.trimmed().isEmpty() ||
        artifactId.trimmed().isEmpty()) {
        const QString error =
            tr("The staged device media artifact cannot be claimed");
        emit artifactClaimFailed(
            operationId, artifactId, error);
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("ClaimDeviceMediaArtifact"),
            {operationId, artifactId},
            operationId,
            QStringLiteral("ClaimDeviceMediaArtifact"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("ClaimDeviceMediaArtifact"),
            operationId, artifactId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, operationId, artifactId]() {
            QDBusPendingReply<TryxRuntimeDeviceMediaArtifact> reply =
                *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    tr("The runtime changed before the artifact was claimed"));
                return;
            }
            if (!reply.isValid()) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    reply.error().message());
                return;
            }
            const TryxRuntimeDeviceMediaArtifact artifact =
                reply.value();
            if (artifact.schemaVersion != 1U ||
                artifact.operationId != operationId ||
                artifact.artifactId != artifactId ||
                artifact.leaseId.isEmpty() ||
                artifact.localPath.isEmpty()) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    tr("The runtime returned an invalid artifact claim"));
                return;
            }
            emit artifactClaimed(operationId, artifact);
        });
}

void RuntimeClient::renewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId) {
    if (!serviceAvailable_ || !compatible_ ||
        artifactId.isEmpty() || leaseId.isEmpty()) {
        emit artifactLeaseRenewFailed(
            artifactId, leaseId,
            tr("The artifact lease cannot be renewed"));
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("RenewDeviceMediaArtifactLease"),
            {artifactId, leaseId},
            {},
            QStringLiteral("RenewDeviceMediaArtifactLease"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("RenewDeviceMediaArtifactLease"),
            artifactId, leaseId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, artifactId, leaseId]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactLeaseRenewFailed(
                    artifactId, leaseId,
                    tr("The runtime changed before the lease was renewed"));
                return;
            }
            if (!reply.isValid() || !reply.value()) {
                emit artifactLeaseRenewFailed(
                    artifactId, leaseId,
                    reply.isValid()
                        ? tr("The runtime rejected the artifact lease renewal")
                        : reply.error().message());
                return;
            }
            emit artifactLeaseRenewed(artifactId, leaseId);
        });
}

void RuntimeClient::releaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId) {
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        return;
    }
    if (!serviceAvailable_ || !compatible_) {
        emit artifactReleaseFailed(
            artifactId, leaseId,
            tr("The runtime is unavailable"));
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("ReleaseDeviceMediaArtifact"),
            {artifactId, leaseId},
            {},
            QStringLiteral("ReleaseDeviceMediaArtifact"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("ReleaseDeviceMediaArtifact"),
            artifactId, leaseId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, artifactId, leaseId]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactReleaseFailed(
                    artifactId, leaseId,
                    tr("The runtime changed before the artifact was released"));
                return;
            }
            if (!reply.isValid() || !reply.value()) {
                emit artifactReleaseFailed(
                    artifactId, leaseId,
                    reply.isValid()
                        ? tr("The runtime rejected the artifact release")
                        : reply.error().message());
                return;
            }
            emit artifactReleased(artifactId, leaseId);
        });
}

QString RuntimeClient::queueRecoveredMediaUploadWithTransform(
    const QString &artifactId, const QString &leaseId,
    const TryxRuntimeMediaTransform &transform) {
    return queueRecoveredMediaUploadWithPreparationProfile(
        artifactId, leaseId,
        tryxFullFrameMediaPreparationProfile(transform));
}

QString RuntimeClient::queueRecoveredMediaUploadWithPreparationProfile(
    const QString &artifactId, const QString &leaseId,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (!mutationReady(tr("Save as new"))) {
        return {};
    }
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        setDiagnostic(tr("The recovered media artifact is unavailable"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    QString profileError;
    if (!tryxMediaPreparationProfileV1IsValid(
            profile, &profileError)) {
        setDiagnostic(tr("Media preparation profile is invalid: %1")
                          .arg(profileError));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const bool split =
        profile.target == QStringLiteral("SplitArea");
    const bool profileContract = hasRuntimeCapability(
        tryxRuntimeMediaPreparationProfileV1Token());
    if (split &&
        (!profileContract ||
         !hasDeviceCapability(
             tryxDeviceDisplayConfigurationV1Token()) ||
         !hasDeviceCapability(
             tryxDeviceMediaSplitAreaV1Token()))) {
        setDiagnostic(tr(
            "Split-area media preparation is not supported by the connected runtime and device"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    if (profileContract) {
        sendOperation(
            QStringLiteral(
                "QueueRecoveredMediaUploadWithPreparationProfileV1"),
            {operationId, artifactId, leaseId,
             QVariant::fromValue(profile)},
            operationId, QStringLiteral("RecoveredMediaUpload"));
        return operationId;
    }
    sendOperation(
        QStringLiteral("QueueRecoveredMediaUploadWithTransform"),
        {operationId, artifactId, leaseId,
         QVariant::fromValue(profile.transform)},
        operationId, QStringLiteral("RecoveredMediaUpload"));
    return operationId;
}

QString RuntimeClient::queueReplaceDeviceMedia(
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform) {
    return queueReplaceDeviceMediaWithPreparationProfile(
        artifactId, leaseId, originalMediaId,
        request,
        tryxFullFrameMediaPreparationProfile(transform));
}

QString RuntimeClient::queueReplaceDeviceMediaWithPreparationProfile(
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (!mutationReady(tr("Replace"))) {
        return {};
    }
    const QString badgeBlock = displayMediaReplacementBlockReason();
    if (!badgeBlock.isEmpty()) {
        setDiagnostic(badgeBlock);
        emit userMessage(badgeBlock, true);
        return {};
    }
    if (artifactId.isEmpty() || leaseId.isEmpty() ||
        originalMediaId.isEmpty()) {
        setDiagnostic(tr("The recovered media replacement is unavailable"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    QString profileError;
    if (!tryxMediaPreparationProfileV1IsValid(
            profile, &profileError)) {
        setDiagnostic(tr("Media preparation profile is invalid: %1")
                          .arg(profileError));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const bool split =
        profile.target == QStringLiteral("SplitArea");
    const bool profileContract = hasRuntimeCapability(
        tryxRuntimeMediaPreparationProfileV1Token());
    if (split &&
        (!profileContract ||
         !hasDeviceCapability(
             tryxDeviceDisplayConfigurationV1Token()) ||
         !hasDeviceCapability(
             tryxDeviceMediaSplitAreaV1Token()))) {
        setDiagnostic(tr(
            "Split-area media preparation is not supported by the connected runtime and device"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    if (profileContract) {
        sendOperation(
            QStringLiteral(
                "QueueReplaceDeviceMediaWithPreparationProfileV1"),
            {operationId, artifactId, leaseId,
             originalMediaId, QVariant::fromValue(request),
             QVariant::fromValue(profile)},
            operationId, QStringLiteral("ReplaceDeviceMedia"));
        return operationId;
    }
    sendOperation(
        QStringLiteral("QueueReplaceDeviceMedia"),
        {operationId, artifactId, leaseId, originalMediaId,
         QVariant::fromValue(request),
         QVariant::fromValue(profile.transform)},
        operationId, QStringLiteral("ReplaceDeviceMedia"));
    return operationId;
}

QString RuntimeClient::displayMediaReplacementBlockReason() const {
    if (!capabilitiesReady_ || (usesDisplaySnapshotV1() && !displayStateValid())) {
        return tr("Replace requires a fresh active layout that references the original media");
    }
    // The frozen Replace contract cannot carry badge choices. Never clone a
    // Custom layout through its Auto-only request or replacement journal.
    if (usesDisplaySnapshotV1() && tryxOverlayBadgesHaveCustomText(displaySnapshot_.badges)) {
        return tr("Replace cannot preserve custom badge text. Save a new copy, then select it and use Apply.");
    }
    return {};
}

TryxRuntimeApplyRequest
RuntimeClient::currentDisplayApplyRequest() const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = display_.media;
    request.screenMode = display_.screenMode.isEmpty()
        ? QStringLiteral("Full Screen")
        : display_.screenMode;
    request.playMode = display_.playMode.isEmpty()
        ? QStringLiteral("Single")
        : display_.playMode;
    request.sysinfoLabels = display_.sysinfoLabels;
    request.settingsBadges = display_.settingsBadges;
    request.sysinfoLabels2 = display_.sysinfoLabels2;
    request.settingsBadges2 = display_.settingsBadges2;
    request.settingsPosition2 =
        display_.settingsPosition2.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition2;
    request.settingsColor2 = displayRightColor();
    request.settingsAlign2 = display_.settingsAlign2.isEmpty()
        ? QStringLiteral("Right")
        : display_.settingsAlign2;
    request.replaceOverlay = true;
    return request;
}

QString RuntimeClient::queueCacheCleanup() {
    if (!cacheCleanupOperationId_.isEmpty()) {
        setDiagnostic(tr(
            "A temporary file cleanup request is already tracked"));
        return {};
    }
    if (!serviceAvailable_ || !compatible_ ||
        !hasRuntimeCapability(tryxRuntimeCacheCleanupV1Token()) ||
        operationBusy()) {
        setDiagnostic(tr(
            "Temporary file cleanup is unavailable from the active runtime"));
        return {};
    }

    const QString owner = offline_
        ? QStringLiteral(":offline.cache-cleanup")
        : runtimeOwner_;
    if (!offline_ && !handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, owner)) {
        setDiagnostic(tr(
            "The runtime changed before temporary file cleanup could start"));
        return {};
    }

    const QString operationId = nextOperationId();
    cacheCleanupOperationId_ = operationId;
    cacheCleanupOwner_ = owner;
    sendOperation(
        QStringLiteral("QueueCacheCleanupV1"), {operationId},
        operationId, QStringLiteral("CacheCleanup"));
    return operationId;
}

bool RuntimeClient::cancelCacheCleanup(
    const QString &operationId) {
    if (operationId.isEmpty() ||
        operationId != cacheCleanupOperationId_ ||
        cacheCleanupOwner_.isEmpty()) {
        return false;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("CancelOperation"), {operationId},
            operationId, QStringLiteral("CacheCleanupCancel"),
        });
        return true;
    }
    const QString owner = cacheCleanupOwner_;
    if (runtimeOwner_ != owner || currentRuntimeOwner() != owner) {
        return false;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QStringLiteral("CancelOperation"));
    message.setArguments({operationId});
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, owner]() {
            const QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (cacheCleanupOperationId_ != operationId ||
                cacheCleanupOwner_ != owner ||
                currentRuntimeOwner() != owner) {
                return;
            }
            if (!reply.isValid()) {
                setDiagnostic(reply.error().message());
            }
        });
    return true;
}

bool RuntimeClient::refreshCacheCleanup(
    const QString &operationId) {
    if (operationId.isEmpty() ||
        operationId != cacheCleanupOperationId_ ||
        cacheCleanupOwner_.isEmpty()) {
        return false;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("GetOperation"), {operationId},
            operationId, QStringLiteral("CacheCleanupRefresh"),
        });
        return true;
    }
    const QString owner = cacheCleanupOwner_;
    if (runtimeOwner_ != owner || currentRuntimeOwner() != owner) {
        const QString message = tr(
            "The original runtime owner is no longer available");
        emit cacheCleanupRefreshFailed(operationId, message);
        return false;
    }

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetOperation"), operationId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, owner]() {
            const QDBusPendingReply<TryxRuntimeOperationInfo> reply =
                *watcher;
            watcher->deleteLater();
            if (cacheCleanupOperationId_ != operationId ||
                cacheCleanupOwner_ != owner) {
                return;
            }
            if (currentRuntimeOwner() != owner) {
                emit cacheCleanupRefreshFailed(
                    operationId,
                    tr("The original runtime owner changed during refresh"));
                return;
            }
            if (!reply.isValid() ||
                reply.value().id != operationId ||
                reply.value().kind != QStringLiteral("CacheCleanup")) {
                emit cacheCleanupRefreshFailed(
                    operationId,
                    reply.isValid()
                        ? tr("The runtime returned a different cleanup operation")
                        : reply.error().message());
                return;
            }
            emit cacheCleanupRefreshResolved(reply.value());
        });
    return true;
}

void RuntimeClient::clearCacheCleanupTracking(
    const QString &operationId) {
    if (operationId != cacheCleanupOperationId_) {
        return;
    }
    cacheCleanupOperationId_.clear();
    cacheCleanupOwner_.clear();
}

void RuntimeClient::refreshAll() {
    connectionRevisionRefreshes_ = 0;
    if (!serviceAvailable_) {
        setDiagnostic(tr("Runtime service is not running"));
        return;
    }
    if (!compatible_) {
        startHandshake();
        return;
    }
    if (!capabilitiesReady_ &&
        !runtimeCapabilitiesPending_ &&
        handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        startRuntimeCapabilitiesHandshake(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_);
    }
    if (!metricsCatalogReady_ && !metricsCatalogPending_ &&
        handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        requestMetricsCatalog(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_);
    }
    if (capabilitiesReady_ &&
        runtimeCapabilities_.contains(
            tryxRuntimePresentationPreferencesV1Token()) &&
        !presentationPreferencesPending_ &&
        !presentationPreferencesBusy_) {
        requestPresentationPreferences(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_);
    }
    if (savedLayoutsSupported() && !savedLayoutsBusy_) {
        requestSavedLayouts(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_);
    }
    refreshConnection();
    refreshOperations();
    refreshMedia();
    refreshMetrics();
    refreshDisplay();
}

void RuntimeClient::refreshMedia() {
    if (!compatible_) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RefreshMediaList"));
    if (legacyConnected()) {
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetMediaCatalog")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeMediaCatalogSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                mediaModel_.applySnapshot(reply.value());
            });
}

void RuntimeClient::connectDevice(const QString &port) {
    if (!manager1Ready(tr("Connect"), false)) {
        return;
    }
    const QString normalized = port.trimmed();
    if (!normalized.isEmpty() &&
        (!normalized.startsWith(QStringLiteral("/dev/ttyACM")) ||
         normalized.contains(QStringLiteral("/../")))) {
        setDiagnostic(
            tr("Connect accepts Auto or a /dev/ttyACM device"));
        emit userMessage(diagnostic_, true);
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("ConnectDevice"),
                 {normalized});
}

void RuntimeClient::disconnectDevice() {
    if (!manager1Ready(tr("Disconnect"))) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("DisconnectDevice"));
}

void RuntimeClient::requestDeviceInfo() {
    if (!manager1Ready(tr("Device information"))) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RequestDeviceInfo"));
}

void RuntimeClient::setRotation(int degrees) {
    if (!manager1Ready(tr("Rotation")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "The legacy rotation command is unavailable for printer-class devices"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    int normalized = degrees % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    if ((normalized % 90) != 0) {
        setDiagnostic(tr(
            "Legacy rotation must be 0, 90, 180 or 270 degrees"));
        emit userMessage(diagnostic_, true);
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("SetRotation"),
                 {normalized});
}

void RuntimeClient::rebootDevice() {
    if (!manager1Ready(tr("Reboot")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "The legacy reboot command is unavailable for printer-class devices"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RebootDevice"));
}

void RuntimeClient::startKeepalive(int intervalSec) {
    if (!manager1Ready(tr("Keepalive")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "Legacy keepalive is unavailable for printer-class devices"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("StartKeepalive"),
                 {qBound(5, intervalSec, 60)});
}

void RuntimeClient::stopKeepalive() {
    if (!manager1Ready(tr("Keepalive")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "Legacy keepalive is unavailable for printer-class devices"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("StopKeepalive"));
}

void RuntimeClient::applyFullScreen(
    const QStringList &media, const QString &playMode,
    const QStringList &metrics, const QStringList &badges,
    const QString &position, const QString &color,
    const QString &alignment) {
    if (!mutationReady(tr("Apply"))) {
        return;
    }
    QString metricsError;
    QString badgesError;
    QString styleError;
    if (media.size() != 1 ||
        !isPlayMode(playMode, false) ||
        !metricsSelectionValid(
            metrics, true, MetricArea::Full,
            &metricsError) ||
        !badgesSelectionValid(
            badges, &badgesError) ||
        !overlayStyleValid(
            position, color, alignment, &styleError)) {
        const QString selectionError =
            !metricsError.isEmpty()
            ? metricsError
            : !badgesError.isEmpty()
              ? badgesError
              : styleError;
        setDiagnostic(tr(
            "Full-screen mode requires one media file, a supported play mode and valid metric, badge and overlay style selections%1")
                          .arg(selectionError.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") +
                                         selectionError));
        emit userMessage(diagnostic_, true);
        return;
    }
    const TryxRuntimeApplyRequest request =
        fullScreenApplyRequest(
            media, playMode, metrics, badges,
            position, color, alignment);
    if (legacyConnected()) {
        sendLegacyScreenConfig(request);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApplyWithMetrics"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Apply"));
}

void RuntimeClient::applySplitScreen(
    const QString &leftMedia, const QString &rightMedia,
    const QString &playMode, const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges,
    const QString &leftPosition,
    const QString &leftColor,
    const QString &leftAlignment,
    const QString &rightPosition,
    const QString &rightColor,
    const QString &rightAlignment) {
    if (!mutationReady(tr("Apply"))) {
        return;
    }
    QString leftMetricsError;
    QString rightMetricsError;
    QString leftBadgesError;
    QString rightBadgesError;
    QString leftStyleError;
    QString rightStyleError;
    if (leftMedia.isEmpty() || rightMedia.isEmpty() ||
        leftMedia == rightMedia ||
        !metricsSelectionValid(
            leftMetrics, true, MetricArea::Left,
            &leftMetricsError) ||
        !metricsSelectionValid(
            rightMetrics, true, MetricArea::Right,
            &rightMetricsError) ||
        !badgesSelectionValid(
            leftBadges, &leftBadgesError) ||
        !badgesSelectionValid(
            rightBadges, &rightBadgesError) ||
        !overlayStyleValid(
            leftPosition, leftColor, leftAlignment,
            &leftStyleError) ||
        !overlayStyleValid(
            rightPosition, rightColor, rightAlignment,
            &rightStyleError) ||
        !isPlayMode(playMode, true)) {
        const QString selectionError =
            !leftMetricsError.isEmpty()
            ? leftMetricsError
            : !rightMetricsError.isEmpty()
              ? rightMetricsError
              : !leftBadgesError.isEmpty()
                ? leftBadgesError
                : !rightBadgesError.isEmpty()
                  ? rightBadgesError
                  : !leftStyleError.isEmpty()
                    ? leftStyleError
                    : rightStyleError;
        setDiagnostic(tr(
            "Split-screen mode requires two different media files, Single play mode and valid metric, badge and overlay style selections per side%1")
                          .arg(selectionError.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") +
                                         selectionError));
        emit userMessage(diagnostic_, true);
        return;
    }
    const TryxRuntimeApplyRequest request =
        splitScreenApplyRequest(
            leftMedia, rightMedia, leftMetrics, rightMetrics,
            leftBadges, rightBadges,
            leftPosition, leftColor, leftAlignment,
            rightPosition, rightColor, rightAlignment);
    if (legacyConnected()) {
        if (request.settingsPosition != request.settingsPosition2 ||
            request.settingsColor != request.settingsColor2 ||
            request.settingsAlign != request.settingsAlign2) {
            setDiagnostic(tr(
                "Legacy split-screen mode requires the same overlay style on both sides"));
            emit userMessage(diagnostic_, true);
            return;
        }
        sendLegacyScreenConfig(request);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApplyWithMetrics"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Apply"));
}

QString RuntimeClient::submitFullDisplayDraft(
    const QStringList &media, const QString &playMode,
    const QStringList &metrics, const QStringList &badges,
    const QString &position, const QString &color,
    const QString &alignment, bool layoutPresent,
    bool brightnessPresent, int brightness,
    bool orientationPresent, bool mirror, bool waterfall, const QVariantMap &badgeChoices) {
    if (!mutationReady(tr("Apply"))) {
        return {};
    }
    if (!layoutPresent && !brightnessPresent &&
        !orientationPresent) {
        setDiagnostic(tr("There are no display changes to apply"));
        emit userMessage(diagnostic_, true);
        return {};
    }

    if (layoutPresent) {
        QString metricsError;
        QString badgesError;
        QString styleError;
        if (media.size() != 1 ||
            !isPlayMode(playMode, false) ||
            !metricsSelectionValid(
                metrics, true, MetricArea::Full,
                &metricsError) ||
            !badgesSelectionValid(
                badges, &badgesError) ||
            !overlayStyleValid(
                position, color, alignment, &styleError)) {
            const QString selectionError =
                !metricsError.isEmpty()
                ? metricsError
                : !badgesError.isEmpty()
                  ? badgesError
                  : styleError;
            setDiagnostic(tr(
                "Full-screen mode requires one media file, a supported play mode and valid metric, badge and overlay style selections%1")
                              .arg(selectionError.isEmpty()
                                       ? QString()
                                       : QStringLiteral(": ") +
                                             selectionError));
            emit userMessage(diagnostic_, true);
            return {};
        }
    }

    TryxRuntimeApplyRequest request = layoutPresent
        ? fullScreenApplyRequest(
              media, playMode, metrics, badges,
              position, color, alignment)
        : legacyConnected()
          ? currentDisplayApplyRequest()
          : TryxRuntimeApplyRequest{};
    request.display.brightnessPresent = brightnessPresent;
    request.display.brightness = qBound(0, brightness, 100);
    request.display.orientationPresent = orientationPresent;
    request.display.mirrorMode = mirror;
    request.display.waterfallMode = waterfall;
    if (orientationPresent) {
        request.waterfallMode = waterfall;
    }

    if (!legacyConnected()) {
        return beginDisplaySubmission(
            request, layoutPresent, brightnessPresent,
            orientationPresent, false, false,
            layoutPresent
                ? QStringLiteral("QueueApplyWithMetrics")
                : QStringLiteral("QueueApply"), {}, 0, badgeChoices);
    }

    const bool mirrorChanged =
        orientationPresent && mirror != display_.mirrorMode;
    const bool waterfallChanged =
        orientationPresent &&
        waterfall != display_.waterfallMode;
    const bool screenConfig =
        layoutPresent || waterfallChanged;
    if (mirrorChanged) {
        setDiagnostic(tr(
            "Legacy display apply cannot change mirror mode"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (screenConfig && brightnessPresent) {
        setDiagnostic(tr(
            "Legacy display apply cannot combine layout or waterfall changes with brightness"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (!screenConfig && !brightnessPresent) {
        setDiagnostic(tr(
            "There are no supported legacy display changes to apply"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (screenConfig && !layoutPresent &&
        (!legacyLayoutConfirmed_ ||
         legacyDisplayStateIdentity_ !=
             legacyDisplayIdentity())) {
        setDiagnostic(tr(
            "Legacy waterfall changes require a confirmed display layout"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    return beginDisplaySubmission(
        request, layoutPresent, brightnessPresent,
        orientationPresent, screenConfig,
        brightnessPresent,
        screenConfig
            ? QStringLiteral("SetScreenConfig")
            : QStringLiteral("SetBrightness"), {}, 0, badgeChoices);
}

QString RuntimeClient::submitSplitDisplayDraft(
    const QString &leftMedia, const QString &rightMedia,
    const QString &playMode, const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges,
    const QString &leftPosition,
    const QString &leftColor,
    const QString &leftAlignment,
    const QString &rightPosition,
    const QString &rightColor,
    const QString &rightAlignment,
    bool layoutPresent, bool brightnessPresent,
    int brightness, bool orientationPresent,
    bool mirror, bool waterfall, const QVariantMap &badgeChoices) {
    if (!mutationReady(tr("Apply"))) {
        return {};
    }
    if (!layoutPresent && !brightnessPresent &&
        !orientationPresent) {
        setDiagnostic(tr("There are no display changes to apply"));
        emit userMessage(diagnostic_, true);
        return {};
    }

    if (layoutPresent) {
        QString leftMetricsError;
        QString rightMetricsError;
        QString leftBadgesError;
        QString rightBadgesError;
        QString leftStyleError;
        QString rightStyleError;
        if (leftMedia.isEmpty() || rightMedia.isEmpty() ||
            leftMedia == rightMedia ||
            !metricsSelectionValid(
                leftMetrics, true, MetricArea::Left,
                &leftMetricsError) ||
            !metricsSelectionValid(
                rightMetrics, true, MetricArea::Right,
                &rightMetricsError) ||
            !badgesSelectionValid(
                leftBadges, &leftBadgesError) ||
            !badgesSelectionValid(
                rightBadges, &rightBadgesError) ||
            !overlayStyleValid(
                leftPosition, leftColor, leftAlignment,
                &leftStyleError) ||
            !overlayStyleValid(
                rightPosition, rightColor, rightAlignment,
                &rightStyleError) ||
            !isPlayMode(playMode, true)) {
            const QString selectionError =
                !leftMetricsError.isEmpty()
                ? leftMetricsError
                : !rightMetricsError.isEmpty()
                  ? rightMetricsError
                  : !leftBadgesError.isEmpty()
                    ? leftBadgesError
                    : !rightBadgesError.isEmpty()
                      ? rightBadgesError
                      : !leftStyleError.isEmpty()
                        ? leftStyleError
                        : rightStyleError;
            setDiagnostic(tr(
                "Split-screen mode requires two different media files, Single play mode and valid metric, badge and overlay style selections per side%1")
                              .arg(selectionError.isEmpty()
                                       ? QString()
                                       : QStringLiteral(": ") +
                                             selectionError));
            emit userMessage(diagnostic_, true);
            return {};
        }
    }

    TryxRuntimeApplyRequest request = layoutPresent
        ? splitScreenApplyRequest(
              leftMedia, rightMedia,
              leftMetrics, rightMetrics,
              leftBadges, rightBadges,
              leftPosition, leftColor, leftAlignment,
              rightPosition, rightColor, rightAlignment)
        : legacyConnected()
          ? currentDisplayApplyRequest()
          : TryxRuntimeApplyRequest{};
    request.display.brightnessPresent = brightnessPresent;
    request.display.brightness = qBound(0, brightness, 100);
    request.display.orientationPresent = orientationPresent;
    request.display.mirrorMode = mirror;
    request.display.waterfallMode = waterfall;
    if (orientationPresent) {
        request.waterfallMode = waterfall;
    }

    if (!legacyConnected()) {
        return beginDisplaySubmission(
            request, layoutPresent, brightnessPresent,
            orientationPresent, false, false,
            layoutPresent
                ? QStringLiteral("QueueApplyWithMetrics")
                : QStringLiteral("QueueApply"), {}, 0, badgeChoices);
    }

    if (layoutPresent &&
        (request.settingsPosition !=
             request.settingsPosition2 ||
         request.settingsColor != request.settingsColor2 ||
         request.settingsAlign != request.settingsAlign2)) {
        setDiagnostic(tr(
            "Legacy split-screen mode requires the same overlay style on both sides"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const bool mirrorChanged =
        orientationPresent && mirror != display_.mirrorMode;
    const bool waterfallChanged =
        orientationPresent &&
        waterfall != display_.waterfallMode;
    const bool screenConfig =
        layoutPresent || waterfallChanged;
    if (mirrorChanged) {
        setDiagnostic(tr(
            "Legacy display apply cannot change mirror mode"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (screenConfig && brightnessPresent) {
        setDiagnostic(tr(
            "Legacy display apply cannot combine layout or waterfall changes with brightness"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (!screenConfig && !brightnessPresent) {
        setDiagnostic(tr(
            "There are no supported legacy display changes to apply"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    if (screenConfig && !layoutPresent &&
        (!legacyLayoutConfirmed_ ||
         legacyDisplayStateIdentity_ !=
             legacyDisplayIdentity())) {
        setDiagnostic(tr(
            "Legacy waterfall changes require a confirmed display layout"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    return beginDisplaySubmission(
        request, layoutPresent, brightnessPresent,
        orientationPresent, screenConfig,
        brightnessPresent,
        screenConfig
            ? QStringLiteral("SetScreenConfig")
            : QStringLiteral("SetBrightness"), {}, 0, badgeChoices);
}

void RuntimeClient::abandonDisplaySubmission(
    const QString &submissionId) {
    if (!displaySubmission_.active() ||
        !displaySubmission_.unresolved ||
        !displaySubmission_.resultEmitted ||
        displaySubmission_.id != submissionId) {
        return;
    }
    if (displaySubmission_.legacyScreenConfig) {
        pendingLegacyScreenConfig_ = {};
        pendingLegacyScreenConfigValid_ = false;
    }
    displayApplyDeadline_.stop();
    displaySubmission_ = {};
    emit operationChanged();
}

void RuntimeClient::deleteMedia(const QStringList &media) {
    if (!mutationReady(tr("Delete"))) {
        return;
    }
    if (media.size() != 1) {
        setDiagnostic(tr("Select exactly one deletable media file"));
        emit userMessage(diagnostic_, true);
        return;
    }
    if (legacyConnected()) {
        if (!mediaModel_.canDelete(media.constFirst())) {
            setDiagnostic(
                mediaModel_.deleteBlockReason(
                    media.constFirst()));
            emit userMessage(diagnostic_, true);
            return;
        }
        sendVoidCall(tryxRuntimeInterfaceName(),
                     QStringLiteral("DeleteMedia"),
                     {media});
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueDeleteMedia"),
                  {operationId, media}, operationId,
                  QStringLiteral("Delete"));
}

void RuntimeClient::cancelActiveOperation() {
    if (legacyUpload_.active()) {
        setDiagnostic(tr(
            "A legacy upload cannot be cancelled safely while the device worker owns the protected source"));
        emit userMessage(diagnostic_, true);
        return;
    }
    if (!compatible_ || activeOperationId_.isEmpty()) {
        return;
    }
    sendVoidCall(tryxRuntimeOperationsInterfaceName(),
                 QStringLiteral("CancelOperation"),
                 {activeOperationId_});
}

void RuntimeClient::retryOperation(
    const QString &sourceOperationId) {
    if (!mutationReady(tr("Retry"))) {
        return;
    }
    if (sourceOperationId.trimmed().isEmpty()) {
        setDiagnostic(tr("The source operation identity is empty"));
        emit userMessage(diagnostic_, true);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("RetryOperation"),
                  {sourceOperationId, operationId}, operationId,
                  QStringLiteral("Retry"));
}

void RuntimeClient::configureMetrics(
    bool enabled, const QStringList &metrics,
    const QString &alignment, const QString &color) {
    if (!mutationReady(tr("Metrics"))) {
        return;
    }
    if (legacyConnected()) {
        setDiagnostic(tr(
            "Legacy overlay metrics are applied together with the display layout"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeMetricsConfigRequest request;
    QString validationError;
    if (!metricsConfigRequest(
            enabled, metrics, alignment, color,
            &request, &validationError)) {
        setDiagnostic(validationError);
        emit userMessage(diagnostic_, true);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueMetricsConfig"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Metrics"));
}

void RuntimeClient::setBrightness(int value) {
    if (legacyConnected()) {
        if (!mutationReady(tr("Brightness"))) {
            return;
        }
        sendVoidCall(tryxRuntimeInterfaceName(),
                     QStringLiteral("SetBrightness"),
                     {qBound(0, value, 100)});
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.brightnessPresent = true;
    mutation.brightness = qBound(0, value, 100);
    applyDisplayMutation(mutation);
}

void RuntimeClient::setBacklight(bool enabled) {
    if (legacyConnected()) {
        Q_UNUSED(enabled);
        setDiagnostic(tr(
            "Display backlight control is unavailable on the legacy protocol"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.backlightPresent = true;
    mutation.backlightEnabled = enabled;
    applyDisplayMutation(mutation);
}

void RuntimeClient::setOrientation(bool mirror, bool waterfall) {
    if (legacyConnected()) {
        Q_UNUSED(mirror);
        Q_UNUSED(waterfall);
        setDiagnostic(tr(
            "Use the legacy rotation control for a serial/ADB device"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.orientationPresent = true;
    mutation.mirrorMode = mirror;
    mutation.waterfallMode = waterfall;
    applyDisplayMutation(mutation);
}

void RuntimeClient::retranslate() {
    emit connectionChanged();
    emit diagnosticChanged();
    emit operationChanged();
    emit metricsChanged();
    emit displayChanged();
    emit presentationPreferencesChanged();
    emit savedLayoutsChanged();
}

void RuntimeClient::onServiceRegistered(const QString &) {
    ++serviceEpoch_;
    emit runtimeInvalidated();
    clearRuntimeState();
    serviceAvailable_ = true;
    emit connectionChanged();
    startHandshake();
}

void RuntimeClient::onServiceUnregistered(const QString &) {
    ++serviceEpoch_;
    if (legacyUpload_.active()) {
        rejectLegacyUpload(
            tr("Runtime service stopped during the legacy upload"),
            true);
    }
    emit runtimeInvalidated();
    clearRuntimeState();
    serviceAvailable_ = false;
    setDiagnostic(tr("Runtime service stopped"));
    emit connectionChanged();
}

void RuntimeClient::onOperationChanged(
    TryxRuntimeOperationInfo info, quint64 revision) {
    if (!compatible_ ||
        (!offline_ && !dbusSignalContextIsCurrent())) {
        return;
    }
    if (!operationModel_.upsert(info, revision)) {
        return;
    }
    emit operationUpdated(info);
    updateActiveOperation(info);
    observeDisplaySubmissionOperation(info);
}

void RuntimeClient::onOperationRemoved(
    QString operationId, quint64 revision) {
    if (!compatible_ ||
        (!offline_ && !dbusSignalContextIsCurrent())) {
        return;
    }
    if (!operationModel_.remove(operationId, revision)) {
        return;
    }
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
        activeOperation_ = {};
        emit operationChanged();
    }
    if (displaySubmission_.active() &&
        !displaySubmission_.resultEmitted &&
        !displaySubmission_.terminalSucceeded &&
        !displaySubmission_.legacyScreenConfig &&
        !displaySubmission_.legacyBrightness &&
        displaySubmission_.id == operationId) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            tr("The display apply operation disappeared before a terminal result was observed"),
            true);
    }
}

void RuntimeClient::onMediaCatalogUpdated(
    TryxRuntimeMediaCatalogSnapshot snapshot) {
    if (!compatible_ ||
        (!offline_ && !dbusSignalContextIsCurrent())) {
        return;
    }
    mediaModel_.applySnapshot(snapshot);
}

void RuntimeClient::onMetricsStateUpdated(
    TryxRuntimeMetricsState state) {
    if (!compatible_ || !dbusSignalContextIsCurrent()) {
        return;
    }
    applyMetricsState(state);
}

void RuntimeClient::onDisplayStateUpdated(
    TryxRuntimeDisplayState state) {
    if (!compatible_ || !dbusSignalContextIsCurrent()) {
        return;
    }
    applyDisplayState(state);
}

void RuntimeClient::onDisplaySnapshotChangedV1(quint64 revision) {
    if (compatible_ && dbusSignalContextIsCurrent() && usesDisplaySnapshotV1()
        && revision > displaySnapshot_.revision) refreshDisplaySnapshotV1();
}

void RuntimeClient::onDeviceConnected(
    QString, QString, QString, QString, bool, bool,
    quint64) {
    if (!compatible_) {
        return;
    }
    clearDeviceCapabilityState();
    refreshConnection();
    refreshDisplay();
    refreshMetrics();
}

void RuntimeClient::onDeviceDisconnected(quint64 revision) {
    if (!compatible_) {
        return;
    }
    if (legacyUpload_.active()) {
        rejectLegacyUpload(
            tr("The legacy device disconnected during upload"),
            true);
    }
    if (displaySubmission_.active() &&
        !displaySubmission_.resultEmitted) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            tr("The device disconnected before the display apply result was confirmed"),
            true);
    }
    TryxRuntimeSnapshot disconnected = connection_;
    disconnected.connected = false;
    disconnected.printerClassConnected = false;
    disconnected.displaySessionActive = false;
    disconnected.productId.clear();
    disconnected.serial.clear();
    disconnected.firmware.clear();
    disconnected.appVersion.clear();
    disconnected.mediaFiles.clear();
    disconnected.revision = revision;
    applyConnectionSnapshot(disconnected);
    refreshConnection();
}

void RuntimeClient::onDeviceError(QString message, quint64) {
    if (!compatible_) {
        return;
    }
    clearDeviceCapabilityState();
    if (legacyUpload_.active()) {
        rejectLegacyUpload(message, true);
    }
    setDiagnostic(message);
    refreshConnection();
}

void RuntimeClient::onLegacyBrightnessChanged(
    int value, quint64 revision) {
    if (!compatible_ || !legacyConnected()) {
        return;
    }
    const QString identity = legacyDisplayIdentity();
    if (identity.isEmpty()) {
        return;
    }
    if (displaySubmission_.legacyBrightness &&
        displaySubmission_.deviceIdentity != identity) {
        return;
    }
    const quint64 currentRevision = qMax(
        display_.revision, connection_.revision);
    if (revision <= currentRevision) {
        return;
    }
    const bool sameDisplay =
        legacyDisplayStateIdentity_ == identity;
    TryxRuntimeDisplayState candidate = sameDisplay
        ? display_ : TryxRuntimeDisplayState{};
    candidate.revision = revision;
    candidate.deviceSerial = identity;
    candidate.valid = sameDisplay && legacyLayoutConfirmed_;
    candidate.brightness = qBound(0, value, 100);
    if (displaySubmission_.legacyBrightness &&
        revision >
            displaySubmission_.startingDisplayRevision &&
        !displaySubmission_.legacyRequestAcknowledged) {
        displaySubmission_.legacyCandidateObserved = true;
        displaySubmission_.legacyCandidateState = candidate;
        return;
    }
    display_ = candidate;
    displayRevisionReceived_ = true;
    legacyDisplayStateIdentity_ = identity;
    legacyBrightnessConfirmed_ = true;
    emit displayChanged();
    if (displaySubmission_.legacyBrightness) {
        observeDisplaySubmissionState(display_);
    }
}

void RuntimeClient::onLegacyScreenConfigChanged(
    quint64 revision) {
    if (!compatible_ || !legacyConnected() ||
        !pendingLegacyScreenConfigValid_ ||
        displaySubmission_.resultEmitted ||
        displaySubmission_.deviceIdentity !=
            legacyDisplayIdentity()) {
        return;
    }
    if (!displaySubmission_.legacyScreenConfig ||
        revision <=
            displaySubmission_.startingDisplayRevision) {
        return;
    }
    const TryxRuntimeApplyRequest request =
        pendingLegacyScreenConfig_;
    const QString identity = legacyDisplayIdentity();
    if (identity.isEmpty()) {
        return;
    }
    TryxRuntimeDisplayState candidate =
        legacyDisplayStateIdentity_ == identity
        ? display_ : TryxRuntimeDisplayState{};
    candidate.revision = revision;
    candidate.deviceSerial = identity;
    candidate.valid = true;
    candidate.screenMode = request.screenMode;
    candidate.playMode = request.playMode;
    candidate.media = request.media;
    candidate.sysinfoLabels = request.sysinfoLabels;
    candidate.settingsBadges = request.settingsBadges;
    candidate.settingsPosition = request.settingsPosition;
    candidate.settingsColor = request.settingsColor;
    candidate.settingsAlign = request.settingsAlign;
    candidate.sysinfoLabels2 = request.sysinfoLabels2;
    candidate.settingsBadges2 = request.settingsBadges2;
    candidate.settingsPosition2 = request.settingsPosition2;
    candidate.settingsColor2 = request.settingsColor2;
    candidate.settingsAlign2 = request.settingsAlign2;
    candidate.waterfallMode = request.waterfallMode;
    if (!displaySubmission_.legacyRequestAcknowledged) {
        displaySubmission_.legacyCandidateObserved = true;
        displaySubmission_.legacyCandidateState = candidate;
        return;
    }
    pendingLegacyScreenConfigValid_ = false;
    display_ = candidate;
    displayRevisionReceived_ = true;
    legacyDisplayStateIdentity_ = identity;
    legacyLayoutConfirmed_ = true;
    emit displayChanged();
    if (displaySubmission_.legacyScreenConfig) {
        observeDisplaySubmissionState(display_);
    }
}

void RuntimeClient::onLegacyMediaUploaded(
    QString filename, quint64) {
    if (!compatible_ || !legacyUpload_.active()) {
        return;
    }
    finishLegacyUpload(filename);
    refreshMedia();
}

void RuntimeClient::onLegacyMediaDeleted(quint64) {
    if (compatible_ && legacyConnected()) {
        refreshMedia();
    }
}

void RuntimeClient::onLegacyMediaListUpdated(
    QStringList files, quint64 revision) {
    if (!compatible_) {
        return;
    }
    if (!legacyConnected()) {
        if (printerClassDevicePresent() && !displaySessionActive()) {
            refreshConnection();
        }
        return;
    }
    mediaModel_.applyLegacyFiles(
        files, revision, legacyDeviceIdentity());
}

void RuntimeClient::onLegacyUploadStatus(
    QString status, quint64) {
    if (!compatible_ || status.trimmed().isEmpty()) {
        return;
    }
    setDiagnostic(status);
    if (printerClassDevicePresent() && !displaySessionActive()) {
        refreshConnection();
    }
    if (legacyUpload_.active()) {
        emit operationChanged();
    }
}

void RuntimeClient::onLegacyUploadTimeout() {
    if (!legacyUpload_.active() ||
        legacyUpload_.rejectionEmitted) {
        return;
    }
    legacyUpload_.rejectionEmitted = true;
    const QString operationId =
        legacyUpload_.operationId;
    const QString error = tr(
        "Legacy upload timed out. The protected source is retained until the device worker reports a terminal result.");
    setDiagnostic(error);
    emit operationRequestRejected(
        operationId, QStringLiteral("Upload"), error);
    emit userMessage(error, true);
    emit operationChanged();
}

void RuntimeClient::onPrinterPresenceChanged(
    bool present, bool printerClassConnected, quint64) {
    if (!compatible_) {
        return;
    }
    Q_UNUSED(present)
    Q_UNUSED(printerClassConnected)
    clearDeviceCapabilityState();
    refreshConnection();
}

void RuntimeClient::onPrinterOperationsCancelled(quint64) {
    if (!compatible_) {
        return;
    }
    clearDeviceCapabilityState();
    refreshConnection();
}

void RuntimeClient::onDisplaySessionChanged(
    bool active, quint64) {
    if (!compatible_) {
        return;
    }
    clearDeviceCapabilityState();
    if (!active && displaySubmission_.active() &&
        !displaySubmission_.resultEmitted) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            tr("The display session ended before the display apply result was confirmed"),
            true);
    }
    refreshConnection();
    refreshDisplay();
}

void RuntimeClient::onPresentationPreferencesChangedV1(
    TryxRuntimePresentationPreferencesV1 preferences) {
    if (!calledFromDBus()) {
        return;
    }
    const QDBusMessage signalMessage = message();
    const QString owner = signalMessage.service();
    if (signalMessage.type() != QDBusMessage::SignalMessage ||
        owner.isEmpty() ||
        owner != presentationPreferencesSignalOwner_ ||
        !capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimePresentationPreferencesV1Token()) ||
        !handshakeContextIsCurrent(
            serviceEpoch_, handshakeAttempt_, owner)) {
        return;
    }
    if (applyPresentationPreferencesSnapshot(preferences)) {
        emit presentationPreferencesChanged();
    }
}

void RuntimeClient::subscribeSignals() {
    if (signalsSubscribed_ || !bus_.isConnected()) {
        return;
    }
    const QString service = tryxRuntimeServiceName();
    const QString path = tryxRuntimeObjectPath();
    const QString manager1 = tryxRuntimeInterfaceName();
    const QString manager2 =
        tryxRuntimeOperationsInterfaceName();

    bool ok = true;
    ok &= bus_.connect(
        service, path, manager2, QStringLiteral("OperationChanged"),
        this,
        SLOT(onOperationChanged(TryxRuntimeOperationInfo,quint64)));
    ok &= bus_.connect(
        service, path, manager2, QStringLiteral("OperationRemoved"),
        this, SLOT(onOperationRemoved(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("MediaCatalogUpdated"), this,
        SLOT(onMediaCatalogUpdated(TryxRuntimeMediaCatalogSnapshot)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("MetricsStateUpdated"), this,
        SLOT(onMetricsStateUpdated(TryxRuntimeMetricsState)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("DisplayStateUpdated"), this,
        SLOT(onDisplayStateUpdated(TryxRuntimeDisplayState)));
    ok &= bus_.connect(service, path, manager2, QStringLiteral("DisplaySnapshotChangedV1"), this,
                       SLOT(onDisplaySnapshotChangedV1(quint64)));
    ok &= bus_.connect(
        service, path, manager1, QStringLiteral("DeviceConnected"),
        this,
        SLOT(onDeviceConnected(QString,QString,QString,QString,bool,bool,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("DeviceDisconnected"), this,
        SLOT(onDeviceDisconnected(quint64)));
    ok &= bus_.connect(
        service, path, manager1, QStringLiteral("DeviceError"),
        this, SLOT(onDeviceError(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("BrightnessChanged"), this,
        SLOT(onLegacyBrightnessChanged(int,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("ScreenConfigChanged"), this,
        SLOT(onLegacyScreenConfigChanged(quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaUploaded"), this,
        SLOT(onLegacyMediaUploaded(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaDeleted"), this,
        SLOT(onLegacyMediaDeleted(quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaListUpdated"), this,
        SLOT(onLegacyMediaListUpdated(QStringList,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("UploadStatus"), this,
        SLOT(onLegacyUploadStatus(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("PrinterPresenceChanged"), this,
        SLOT(onPrinterPresenceChanged(bool,bool,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("PrinterOperationsCancelled"), this,
        SLOT(onPrinterOperationsCancelled(quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("DisplaySessionChanged"), this,
        SLOT(onDisplaySessionChanged(bool,quint64)));
    signalsSubscribed_ = ok;
    if (!ok) {
        setDiagnostic(tr("Could not subscribe to all runtime signals"));
    }
}

void RuntimeClient::startHandshake() {
    if (!serviceAvailable_ || handshakePending_) {
        return;
    }

    const QString owner = currentRuntimeOwner();
    if (!owner.startsWith(QLatin1Char(':'))) {
        compatible_ = false;
        setDiagnostic(tr("Could not resolve the runtime service owner"));
        emit connectionChanged();
        return;
    }

    ++handshakeAttempt_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    runtimeOwner_ = owner;
    handshakePending_ = true;
    clearCapabilityState();
    clearMetricsCatalogState();

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetRuntimeApiVersion")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch, handshakeAttempt, owner]() {
                QDBusPendingReply<quint32> reply = *watcher;
                watcher->deleteLater();
                if (epoch == serviceEpoch_ &&
                    handshakeAttempt == handshakeAttempt_) {
                    handshakePending_ = false;
                }
                if (!handshakeContextIsCurrent(
                        epoch, handshakeAttempt, owner)) {
                    return;
                }
                if (!reply.isValid()) {
                    compatible_ = false;
                    setDiagnostic(reply.error().message());
                    emit connectionChanged();
                    return;
                }
                apiVersion_ = reply.value();
                compatible_ =
                    apiVersion_ == tryxRuntimeApiVersion();
                if (!compatible_) {
                    setDiagnostic(tr(
                        "Runtime API %1 is active, but this client requires API %2")
                                      .arg(apiVersion_)
                                      .arg(tryxRuntimeApiVersion()));
                } else {
                    setDiagnostic({});
                }
                emit connectionChanged();
                if (compatible_) {
                    startRuntimeCapabilitiesHandshake(
                        epoch, handshakeAttempt, owner);
                    refreshAll();
                }
            });
}

void RuntimeClient::startRuntimeCapabilitiesHandshake(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) {
    if (runtimeCapabilitiesPending_ || capabilitiesReady_ ||
        !handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        return;
    }

    runtimeCapabilitiesPending_ = true;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetRuntimeCapabilities")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner]() {
            const QDBusPendingReply<QStringList> reply = *watcher;
            watcher->deleteLater();
            if (epoch == serviceEpoch_ &&
                handshakeAttempt == handshakeAttempt_) {
                runtimeCapabilitiesPending_ = false;
            }
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }

            if (!reply.isValid()) {
                const QDBusError error = reply.error();
                const bool legacyRuntime =
                    error.type() == QDBusError::UnknownMethod &&
                    error.name() == QStringLiteral(
                        "org.freedesktop.DBus.Error.UnknownMethod");
                runtimeCapabilitiesFailed_ = !legacyRuntime;
                clearDeviceCapabilityState();
                clearPresentationPreferencesState();
                clearSavedLayoutsState();
                const bool changed =
                    capabilitiesReady_ != legacyRuntime ||
                    !runtimeCapabilities_.isEmpty();
                capabilitiesReady_ = legacyRuntime;
                runtimeCapabilities_.clear();
                if (!legacyRuntime) {
                    setDeviceSpecificationsState(
                        DeviceSpecificationsState::RuntimeUnavailable);
                    setDiagnostic(error.message());
                }
                if (changed) {
                    emit capabilitiesChanged();
                }
                emit displayChanged();
                if (legacyRuntime) refreshDisplay();
                return;
            }

            const QStringList filtered =
                tryxFilterRuntimeCapabilities(reply.value());
            runtimeCapabilitiesFailed_ = false;
            const bool changed = !capabilitiesReady_ ||
                                 runtimeCapabilities_ != filtered;
            capabilitiesReady_ = true;
            runtimeCapabilities_ = filtered;
            if (filtered.contains(tryxRuntimeDisplaySnapshotV1Token())) displaySnapshotRequired_ = true;
            if (changed) {
                emit capabilitiesChanged();
            }
            emit displayChanged();
            if (!usesDisplaySnapshotV1()) refreshDisplay();
            if (runtimeCapabilities_.contains(
                    tryxRuntimeDeviceCapabilitiesV1Token())) {
                requestDeviceCapabilities(
                    epoch, handshakeAttempt, owner);
            } else {
                clearDeviceCapabilityState();
            }
            if (runtimeCapabilities_.contains(
                    tryxRuntimePresentationPreferencesV1Token())) {
                subscribePresentationPreferencesSignal(owner);
                requestPresentationPreferences(
                    epoch, handshakeAttempt, owner);
            } else {
                clearPresentationPreferencesState();
            }
            if (savedLayoutsSupported()) {
                requestSavedLayouts(
                    epoch, handshakeAttempt, owner);
            } else {
                clearSavedLayoutsState();
            }
        });
}

void RuntimeClient::requestMetricsCatalog(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) {
    if (metricsCatalogPending_ || metricsCatalogReady_ ||
        !handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        return;
    }

    metricsCatalogPending_ = true;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetMetricsCapabilities")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner]() {
            const QDBusPendingReply<QStringList> reply = *watcher;
            watcher->deleteLater();
            if (epoch == serviceEpoch_ &&
                handshakeAttempt == handshakeAttempt_) {
                metricsCatalogPending_ = false;
            }
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }

            if (!reply.isValid()) {
                const QDBusError error = reply.error();
                const bool legacyFallback =
                    error.type() == QDBusError::UnknownMethod &&
                    error.name() == QStringLiteral(
                        "org.freedesktop.DBus.Error.UnknownMethod");
                if (!legacyFallback) {
                    clearMetricsCatalogState();
                    setDiagnostic(error.message());
                    return;
                }
                const bool readinessChanged =
                    !metricsCatalogReady_ ||
                    !metricsCatalogLegacyFallback_;
                const QStringList previousCatalog = metricsCatalog_;
                metricsCatalogReady_ = true;
                metricsCatalogLegacyFallback_ = true;
                refreshLegacyMetricsCatalog();
                if (readinessChanged &&
                    previousCatalog == metricsCatalog_) {
                    emit metricsCatalogChanged();
                }
                return;
            }

            const QStringList catalog = reply.value();
            if (!metricsCatalogReplyIsValid(catalog)) {
                clearMetricsCatalogState();
                setDiagnostic(tr(
                    "The runtime returned an invalid metrics catalog"));
                return;
            }
            const bool changed = !metricsCatalogReady_ ||
                                 metricsCatalogLegacyFallback_ ||
                                 metricsCatalog_ != catalog;
            metricsCatalogReady_ = true;
            metricsCatalogLegacyFallback_ = false;
            metricsCatalog_ = catalog;
            if (changed) {
                emit metricsCatalogChanged();
            }
        });
}

void RuntimeClient::requestDeviceCapabilities(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) {
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner) ||
        !capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceCapabilitiesV1Token())) {
        clearDeviceCapabilityState();
        return;
    }

    const QString expectedIdentity =
        connection_.serial.trimmed();
    const quint64 expectedRevision = connection_.revision;
    clearDeviceCapabilityState();
    if (!connection_.connected ||
        !connection_.printerClassConnected ||
        !connection_.printerClassDevicePresent ||
        expectedIdentity.isEmpty() || expectedRevision == 0) {
        return;
    }

    const quint64 deviceAttempt = deviceCapabilitiesAttempt_;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetDeviceCapabilitiesV1")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         deviceAttempt, expectedIdentity, expectedRevision]() {
            const QDBusPendingReply<
                TryxRuntimeDeviceCapabilitiesV1> reply = *watcher;
            watcher->deleteLater();
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner) ||
                deviceAttempt != deviceCapabilitiesAttempt_ ||
                !connection_.connected ||
                !connection_.printerClassConnected ||
                !connection_.printerClassDevicePresent ||
                connection_.revision != expectedRevision ||
                connection_.serial.trimmed() != expectedIdentity) {
                return;
            }
            if (!reply.isValid()) {
                if (runtimeCapabilities_.contains(
                        tryxRuntimeDeviceSpecificationsV1Token())) {
                    setDeviceSpecificationsState(
                        DeviceSpecificationsState::RuntimeUnavailable);
                }
                setDiagnostic(reply.error().message());
                return;
            }

            const TryxRuntimeDeviceCapabilitiesV1 value =
                reply.value();
            if (value.schemaVersion != 1 ||
                value.deviceIdentity.isEmpty() ||
                value.deviceIdentity != expectedIdentity ||
                value.connectionRevision != expectedRevision ||
                value.physicalGeneration == 0) {
                if (runtimeCapabilities_.contains(
                        tryxRuntimeDeviceSpecificationsV1Token())) {
                    setDeviceSpecificationsState(
                        DeviceSpecificationsState::RuntimeUnavailable);
                }
                setDiagnostic(tr(
                    "The runtime returned device capabilities for an invalid device context"));
                if (value.schemaVersion == 1
                    && value.deviceIdentity == expectedIdentity
                    && value.physicalGeneration != 0) {
                    reconcileConnectionRevision(value.connectionRevision);
                }
                return;
            }

            const QStringList filtered =
                tryxFilterDeviceCapabilities(value.capabilities);
            const bool changed = !deviceCapabilitiesReady_ ||
                                 deviceCapabilities_ != filtered;
            deviceCapabilitiesSnapshot_ = value;
            deviceCapabilitiesSnapshot_.capabilities = filtered;
            deviceCapabilities_ = filtered;
            deviceCapabilitiesReady_ = true;
            if (!usesDisplaySnapshotV1()) {
                completeConnectionRevisionReconciliation();
            }
            if (changed) {
                emit capabilitiesChanged();
            }
            if (usesDisplaySnapshotV1()) refreshDisplaySnapshotV1();
            if (runtimeCapabilities_.contains(
                    tryxRuntimeDeviceSpecificationsV1Token())) {
                requestDeviceSpecifications(
                    epoch, handshakeAttempt, owner);
            } else {
                clearDeviceSpecificationsState();
            }
        });
}

bool RuntimeClient::reconcileConnectionRevision(quint64 observedRevision) {
    if (observedRevision <= connection_.revision
        || !handshakeContextIsCurrent(serviceEpoch_, handshakeAttempt_, runtimeOwner_)) {
        return false;
    }
    connectionRevisionDiagnostic_ = diagnostic_;
    if (connectionRevisionRefreshes_ >= kMaximumConnectionRevisionRefreshes) return false;
    // Manager1 also advances its revision for catalog and metrics events.
    // Re-read the whole context instead of accepting a mismatched reply or
    // repeating a device mutation. A continuously changing context stays closed.
    ++connectionRevisionRefreshes_;
    emit displayChanged();
    refreshConnection();
    return true;
}

void RuntimeClient::completeConnectionRevisionReconciliation() {
    connectionRevisionRefreshes_ = 0;
    if (!connectionRevisionDiagnostic_.isEmpty() && diagnostic_ == connectionRevisionDiagnostic_) {
        setDiagnostic({});
    }
    connectionRevisionDiagnostic_.clear();
}

void RuntimeClient::requestDeviceSpecifications(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) {
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner) ||
        !capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceSpecificationsV1Token())) {
        clearDeviceSpecificationsState();
        return;
    }

    clearDeviceSpecificationsState();
    const QString expectedIdentity = connection_.serial.trimmed();
    const quint64 expectedRevision = connection_.revision;
    if (!connection_.connected) {
        return;
    }
    if (!connection_.printerClassConnected) {
        return;
    }
    if (!connection_.printerClassDevicePresent ||
        expectedIdentity.isEmpty() || expectedRevision == 0 ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceCapabilitiesV1Token()) ||
        !deviceCapabilitiesReady_ ||
        deviceCapabilitiesSnapshot_.schemaVersion != 1 ||
        deviceCapabilitiesSnapshot_.deviceIdentity != expectedIdentity ||
        deviceCapabilitiesSnapshot_.connectionRevision != expectedRevision ||
        deviceCapabilitiesSnapshot_.physicalGeneration == 0) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::RuntimeUnavailable);
        return;
    }

    const quint64 expectedGeneration =
        deviceCapabilitiesSnapshot_.physicalGeneration;
    const QString expectedProductId = connection_.productId;
    const quint64 deviceAttempt = deviceCapabilitiesAttempt_;
    const quint64 specificationsAttempt =
        deviceSpecificationsAttempt_;
    deviceSpecificationsPending_ = true;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetDeviceSpecificationsV1")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         deviceAttempt, specificationsAttempt, expectedIdentity,
         expectedRevision, expectedGeneration, expectedProductId]() {
            const QDBusPendingReply<
                TryxRuntimeDeviceSpecificationsV1> reply = *watcher;
            watcher->deleteLater();
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner) ||
                deviceAttempt != deviceCapabilitiesAttempt_ ||
                specificationsAttempt != deviceSpecificationsAttempt_ ||
                !deviceCapabilitiesReady_ ||
                !connection_.connected ||
                !connection_.printerClassConnected ||
                !connection_.printerClassDevicePresent ||
                connection_.serial.trimmed() != expectedIdentity ||
                connection_.revision != expectedRevision ||
                connection_.productId != expectedProductId ||
                deviceCapabilitiesSnapshot_.deviceIdentity !=
                    expectedIdentity ||
                deviceCapabilitiesSnapshot_.connectionRevision !=
                    expectedRevision ||
                deviceCapabilitiesSnapshot_.physicalGeneration !=
                    expectedGeneration) {
                return;
            }

            deviceSpecificationsPending_ = false;
            if (!reply.isValid()) {
                setDeviceSpecificationsState(
                    DeviceSpecificationsState::RuntimeUnavailable);
                setDiagnostic(reply.error().message());
                return;
            }

            const TryxRuntimeDeviceSpecificationsV1 value =
                reply.value();
            const bool commonContextIsValid =
                value.schemaVersion == 1 &&
                !value.deviceIdentity.isEmpty() &&
                value.deviceIdentity == expectedIdentity &&
                value.connectionRevision == expectedRevision &&
                value.physicalGeneration == expectedGeneration;
            const bool readyPayloadIsValid =
                value.status == QStringLiteral("Ready") &&
                deviceReportedProductNameIsSafe(
                    value.reportedProductName) &&
                value.videoOutputWidth >= 1 &&
                value.videoOutputWidth <= 16384 &&
                value.videoOutputHeight >= 1 &&
                value.videoOutputHeight <= 16384 &&
                (value.screenType == QStringLiteral("LCD") ||
                 value.screenType == QStringLiteral("OLED"));
            const bool emptyPayload =
                deviceSpecificationsPayloadIsEmpty(value);
            const bool supportedProduct =
                supportsDeviceSpecifications(expectedProductId);
            const bool statusAndPayloadAreValid = supportedProduct
                ? (readyPayloadIsValid ||
                   (value.status == QStringLiteral("Unavailable") &&
                    emptyPayload))
                : (value.status == QStringLiteral("Unsupported") &&
                   emptyPayload);
            if (!commonContextIsValid ||
                !statusAndPayloadAreValid) {
                setDeviceSpecificationsState(
                    DeviceSpecificationsState::RuntimeUnavailable);
                setDiagnostic(tr(
                    "The runtime returned device specifications for an invalid device context"));
                return;
            }

            if (value.status == QStringLiteral("Ready")) {
                setDeviceSpecificationsState(
                    DeviceSpecificationsState::Ready, value);
            } else if (value.status == QStringLiteral("Unavailable")) {
                setDeviceSpecificationsState(
                    DeviceSpecificationsState::Unavailable);
            } else {
                setDeviceSpecificationsState(
                    DeviceSpecificationsState::Unsupported);
            }
        });
}

void RuntimeClient::requestPresentationPreferences(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner, bool reconciliation) {
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner) ||
        !capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimePresentationPreferencesV1Token())) {
        if (reconciliation && presentationPreferencesBusy_) {
            presentationPreferencesBusy_ = false;
            emit presentationPreferencesChanged();
        }
        return;
    }
    if (presentationPreferencesPending_ && !reconciliation) {
        return;
    }

    presentationPreferencesPending_ = true;
    const quint64 readAttempt =
        ++presentationPreferencesReadAttempt_;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetPresentationPreferencesV1")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         readAttempt, reconciliation]() {
            const QDBusPendingReply<
                TryxRuntimePresentationPreferencesV1> reply = *watcher;
            watcher->deleteLater();
            if (readAttempt != presentationPreferencesReadAttempt_ ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }

            presentationPreferencesPending_ = false;
            bool notify = false;
            if (reconciliation && presentationPreferencesBusy_) {
                presentationPreferencesBusy_ = false;
                notify = true;
            }
            if (!reply.isValid()) {
                setDiagnostic(reply.error().message());
                if (notify) {
                    emit presentationPreferencesChanged();
                }
                return;
            }
            notify = applyPresentationPreferencesSnapshot(
                         reply.value()) ||
                     notify;
            if (notify) {
                emit presentationPreferencesChanged();
            }
        });
}

void RuntimeClient::requestSavedLayouts(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) {
    if (savedLayoutsBusy_ ||
        !handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner) ||
        !savedLayoutsSupported()) {
        return;
    }
    savedLayoutsBusy_ = true;
    const quint64 requestAttempt = ++savedLayoutsAttempt_;
    const bool version2 = savedLayoutsV2Supported();
    const QString method = version2 ? QStringLiteral("GetSavedLayoutsV2") : QStringLiteral("GetSavedLayoutsV1");
    emit savedLayoutsChanged();

    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(method),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, handshakeAttempt, owner,
         requestAttempt, version2]() {
            const auto reply = savedLayoutsReply(watcher, version2);
            watcher->deleteLater();
            if (requestAttempt != savedLayoutsAttempt_ ||
                !handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                return;
            }
            savedLayoutsBusy_ = false;
            if (!reply.isValid()) {
                const QString message = reply.error().message().isEmpty()
                    ? tr("Saved layouts could not be loaded")
                    : reply.error().message();
                setSavedLayoutsUnavailable(message);
                setDiagnostic(message);
                return;
            }
            QString snapshotError;
            if (!applySavedLayoutsSnapshot(
                    reply.value(), &snapshotError)) {
                setDiagnostic(snapshotError);
                return;
            }
            setDiagnostic({});
        });
}

bool RuntimeClient::subscribePresentationPreferencesSignal(
    const QString &owner) {
    if (owner.isEmpty() || !bus_.isConnected()) {
        return false;
    }
    if (presentationPreferencesSignalOwner_ == owner) {
        return true;
    }
    disconnectPresentationPreferencesSignal();
    const bool connected = bus_.connect(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QStringLiteral("PresentationPreferencesChangedV1"), this,
        SLOT(onPresentationPreferencesChangedV1(
            TryxRuntimePresentationPreferencesV1)));
    if (connected) {
        presentationPreferencesSignalOwner_ = owner;
    } else {
        setDiagnostic(tr(
            "Could not subscribe to presentation preference updates"));
    }
    return connected;
}

void RuntimeClient::disconnectPresentationPreferencesSignal() {
    if (presentationPreferencesSignalOwner_.isEmpty() ||
        !bus_.isConnected()) {
        presentationPreferencesSignalOwner_.clear();
        return;
    }
    bus_.disconnect(
        presentationPreferencesSignalOwner_,
        tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QStringLiteral("PresentationPreferencesChangedV1"), this,
        SLOT(onPresentationPreferencesChangedV1(
            TryxRuntimePresentationPreferencesV1)));
    presentationPreferencesSignalOwner_.clear();
}

bool RuntimeClient::applyPresentationPreferencesSnapshot(
    const TryxRuntimePresentationPreferencesV1 &preferences) {
    if (!tryxPresentationPreferencesAreValid(preferences)) {
        setDiagnostic(tr(
            "The runtime returned invalid presentation preferences"));
        return false;
    }
    if (presentationPreferencesReady_) {
        if (preferences.revision <
            presentationPreferences_.revision) {
            return false;
        }
        if (preferences.revision ==
            presentationPreferences_.revision) {
            if (preferences.temperatureUnit !=
                    presentationPreferences_.temperatureUnit ||
                preferences.timeFormat !=
                    presentationPreferences_.timeFormat) {
                setDiagnostic(tr(
                    "The runtime reused a presentation preference revision with different values"));
            }
            return false;
        }
    }

    const bool changed = !presentationPreferencesReady_ ||
        preferences.temperatureUnit !=
            presentationPreferences_.temperatureUnit ||
        preferences.timeFormat != presentationPreferences_.timeFormat ||
        preferences.revision != presentationPreferences_.revision;
    presentationPreferences_ = preferences;
    presentationPreferencesReady_ = true;
    return changed;
}

void RuntimeClient::clearPresentationPreferencesState() {
    disconnectPresentationPreferencesSignal();
    const TryxRuntimePresentationPreferencesV1 defaults;
    const bool changed = presentationPreferencesReady_ ||
        presentationPreferencesBusy_ ||
        presentationPreferences_.temperatureUnit !=
            defaults.temperatureUnit ||
        presentationPreferences_.timeFormat != defaults.timeFormat;
    ++presentationPreferencesReadAttempt_;
    ++presentationPreferencesMutationAttempt_;
    presentationPreferencesPending_ = false;
    presentationPreferencesReady_ = false;
    presentationPreferencesBusy_ = false;
    presentationPreferences_ = defaults;
    if (changed) {
        emit presentationPreferencesChanged();
    }
}

void RuntimeClient::clearSavedLayoutsState() {
    ++savedLayoutsAttempt_;
    lastConfirmedSavedLayouts_ = {};
    lastConfirmedSavedLayoutsReady_ = false;
    setSavedLayoutsUnavailable({});
}

void RuntimeClient::setSavedLayoutsUnavailable(
    const QString &diagnostic) {
    savedLayoutsBusy_ = false;
    savedLayouts_ = {};
    savedLayouts_.status = QStringLiteral("Unavailable");
    savedLayouts_.diagnostic = diagnostic.left(512);
    savedLayoutModel_.clear();
    emit savedLayoutsChanged();
}

QString RuntimeClient::savedLayoutConnectionIdentity() const {
    return tryxSavedLayoutDeviceIdentityIsCanonical(connection_.serial)
        ? connection_.serial
        : QString();
}

bool RuntimeClient::applySavedLayoutsSnapshot(
    const TryxRuntimeSavedLayoutsSnapshotV2 &snapshot,
    QString *errorMessage) {
    const auto fail =
        [this, errorMessage](const QString &message) {
            setSavedLayoutsUnavailable(message);
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };
    if (!savedLayoutsSnapshotIsValid(snapshot)) {
        return fail(tr(
            "The runtime returned an invalid saved-layout snapshot"));
    }
    if (snapshot.status == QStringLiteral("Ready") &&
        (snapshot.deviceIdentity != savedLayoutConnectionIdentity() ||
         snapshot.productId != connection_.productId)) {
        return fail(tr(
            "The runtime returned saved layouts for another device"));
    }
    if (snapshot.status == QStringLiteral("Ready") &&
        lastConfirmedSavedLayoutsReady_ &&
        snapshot.deviceIdentity ==
            lastConfirmedSavedLayouts_.deviceIdentity &&
        snapshot.productId == lastConfirmedSavedLayouts_.productId &&
        (snapshot.revision < lastConfirmedSavedLayouts_.revision ||
         (snapshot.revision == lastConfirmedSavedLayouts_.revision &&
          !(snapshot == lastConfirmedSavedLayouts_)))) {
        return fail(tr(
            "The runtime returned an invalid saved-layout snapshot"));
    }
    savedLayouts_ = snapshot;
    savedLayoutsBusy_ = false;
    if (snapshot.status == QStringLiteral("Ready")) {
        lastConfirmedSavedLayouts_ = snapshot;
        lastConfirmedSavedLayoutsReady_ = true;
        savedLayoutModel_.applyLayoutsV2(snapshot.layouts);
    } else {
        savedLayoutModel_.clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    emit savedLayoutsChanged();
    return true;
}

bool RuntimeClient::applySavedLayoutsSnapshot(const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot, QString *errorMessage) {
    return applySavedLayoutsSnapshot(promoteSavedLayouts(snapshot), errorMessage);
}

bool RuntimeClient::fullSavedLayoutDraftToRequest(
    const QVariantMap &fullDraft,
    TryxRuntimeApplyRequest *request,
    QString *errorMessage) const {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    if (!request ||
        !mapHasExactKeys(
            fullDraft,
            {QStringLiteral("layout"),
             QStringLiteral("brightness"),
             QStringLiteral("orientation")}) ||
        fullDraft.value(QStringLiteral("layout")).metaType().id() !=
            QMetaType::QVariantMap ||
        fullDraft.value(QStringLiteral("orientation")).metaType().id() !=
            QMetaType::QVariantMap) {
        return fail(tr("The saved layout full-state shape is invalid"));
    }
    const QVariantMap layout =
        fullDraft.value(QStringLiteral("layout")).toMap();
    const QVariantMap orientation =
        fullDraft.value(QStringLiteral("orientation")).toMap();
    bool split = false;
    bool mirror = false;
    bool waterfall = false;
    int brightness = 0;
    if (!exactBool(layout, QStringLiteral("split"), &split) ||
        !exactBool(orientation, QStringLiteral("mirror"), &mirror) ||
        !exactBool(
            orientation, QStringLiteral("waterfall"), &waterfall) ||
        !exactInteger(
            fullDraft, QStringLiteral("brightness"), &brightness) ||
        brightness < 0 || brightness > 100 ||
        !mapHasExactKeys(
            orientation,
            {QStringLiteral("mirror"),
             QStringLiteral("waterfall")})) {
        return fail(tr(
            "The saved layout display state is incomplete or invalid"));
    }

    QStringList expectedLayoutKeys = split
        ? QStringList{
              QStringLiteral("split"), QStringLiteral("media"),
              QStringLiteral("playMode"),
              QStringLiteral("leftMetrics"),
              QStringLiteral("rightMetrics"),
              QStringLiteral("leftBadges"),
              QStringLiteral("rightBadges"),
              QStringLiteral("leftPosition"),
              QStringLiteral("leftColor"),
              QStringLiteral("leftAlignment"),
              QStringLiteral("rightPosition"),
              QStringLiteral("rightColor"),
              QStringLiteral("rightAlignment")}
        : QStringList{
              QStringLiteral("split"), QStringLiteral("media"),
              QStringLiteral("playMode"), QStringLiteral("metrics"),
              QStringLiteral("badges"), QStringLiteral("position"),
              QStringLiteral("color"), QStringLiteral("alignment")};
    if (layout.contains(QStringLiteral("badgeChoices"))) {
        if (layout.value(QStringLiteral("badgeChoices")).metaType().id() != QMetaType::QVariantMap)
            return fail(tr("The saved layout badge choices are invalid"));
        expectedLayoutKeys.append(QStringLiteral("badgeChoices"));
    }
    if (!mapHasExactKeys(layout, expectedLayoutKeys)) {
        return fail(tr(
            "The saved layout fields do not match the selected screen mode"));
    }

    TryxRuntimeApplyRequest candidate;
    candidate.ratio = QStringLiteral("2:1");
    candidate.screenMode = split
        ? QStringLiteral("Screen Splitting")
        : QStringLiteral("Full Screen");
    if (!variantStringList(
            layout.value(QStringLiteral("media")),
            &candidate.media) ||
        !exactString(
            layout, QStringLiteral("playMode"),
            &candidate.playMode)) {
        return fail(tr("The saved layout media selection is invalid"));
    }
    if (split) {
        if (!variantStringList(
                layout.value(QStringLiteral("leftMetrics")),
                &candidate.sysinfoLabels) ||
            !variantStringList(
                layout.value(QStringLiteral("rightMetrics")),
                &candidate.sysinfoLabels2) ||
            !variantStringList(
                layout.value(QStringLiteral("leftBadges")),
                &candidate.settingsBadges) ||
            !variantStringList(
                layout.value(QStringLiteral("rightBadges")),
                &candidate.settingsBadges2) ||
            !exactString(
                layout, QStringLiteral("leftPosition"),
                &candidate.settingsPosition) ||
            !exactString(
                layout, QStringLiteral("leftColor"),
                &candidate.settingsColor) ||
            !exactString(
                layout, QStringLiteral("leftAlignment"),
                &candidate.settingsAlign) ||
            !exactString(
                layout, QStringLiteral("rightPosition"),
                &candidate.settingsPosition2) ||
            !exactString(
                layout, QStringLiteral("rightColor"),
                &candidate.settingsColor2) ||
            !exactString(
                layout, QStringLiteral("rightAlignment"),
                &candidate.settingsAlign2)) {
            return fail(tr(
                "The split saved-layout fields are invalid"));
        }
    } else if (!variantStringList(
                   layout.value(QStringLiteral("metrics")),
                   &candidate.sysinfoLabels) ||
               !variantStringList(
                   layout.value(QStringLiteral("badges")),
                   &candidate.settingsBadges) ||
               !exactString(
                   layout, QStringLiteral("position"),
                   &candidate.settingsPosition) ||
               !exactString(
                   layout, QStringLiteral("color"),
                   &candidate.settingsColor) ||
               !exactString(
                   layout, QStringLiteral("alignment"),
                   &candidate.settingsAlign)) {
        return fail(tr(
            "The full-screen saved-layout fields are invalid"));
    }
    candidate.filterOpacity = 0;
    candidate.presetId.clear();
    candidate.waterfallMode = waterfall;
    candidate.replaceOverlay = true;
    candidate.display.brightnessPresent = true;
    candidate.display.brightness = brightness;
    candidate.display.standbyPresent = false;
    candidate.display.standbyEnabled = false;
    candidate.display.orientationPresent = true;
    candidate.display.mirrorMode = mirror;
    candidate.display.waterfallMode = waterfall;
    candidate.display.backlightPresent = false;
    candidate.display.backlightEnabled = true;
    if (!savedApplyRequestIsCanonical(candidate)) {
        return fail(tr(
            "The saved layout contains unsupported media, metrics or display values"));
    }
    if (!metricsCatalogReady_ ||
        !std::all_of(
            candidate.sysinfoLabels.cbegin(),
            candidate.sysinfoLabels.cend(),
            [this](const QString &metric) {
                return metricsCatalog_.contains(metric);
            }) ||
        !std::all_of(
            candidate.sysinfoLabels2.cbegin(),
            candidate.sysinfoLabels2.cend(),
            [this](const QString &metric) {
                return metricsCatalog_.contains(metric);
            })) {
        return fail(tr(
            "The saved layout metrics catalog is not confirmed"));
    }
    *request = candidate;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool RuntimeClient::savedLayoutFromDraft(
    const QString &name, const QString &overwriteLayoutId,
    const QVariantMap &fullDraft,
    TryxRuntimeSavedLayoutV2 *layout,
    QString *errorMessage) const {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    if (!layout || !savedLayoutNameIsValid(name)) {
        return fail(tr(
            "Enter a name from 1 to 80 plain-text characters"));
    }
    TryxRuntimeApplyRequest request;
    if (!fullSavedLayoutDraftToRequest(
            fullDraft, &request, errorMessage)) {
        return false;
    }
    TryxRuntimeSavedLayoutV2 candidate;
    if (!overwriteLayoutId.isEmpty()) {
        TryxRuntimeSavedLayoutV2 existing;
        if (!savedLayoutModel_.layoutV2ById(
                overwriteLayoutId, &existing) ||
            existing.name.compare(name, Qt::CaseInsensitive) != 0) {
            return fail(tr(
                "The selected saved layout cannot be overwritten by this name"));
        }
        candidate.layoutId = existing.layoutId;
        candidate.revision = existing.revision;
        candidate.name = existing.name;
    } else {
        candidate.name = name;
    }
    candidate.deviceIdentity = savedLayouts_.deviceIdentity;
    candidate.productId = savedLayouts_.productId;
    candidate.request = request;
    if (!normalizeBadgeDraft(fullDraft.value(QStringLiteral("layout")).toMap().value(QStringLiteral("badgeChoices")).toMap(),
            request, &candidate.badges)
        || (tryxOverlayBadgesHaveCustomText(candidate.badges) && !customBadgeTextSupported()))
        return fail(tr("Custom badge text is invalid or is not supported by the current runtime and device"));
    for (const QString &mediaName : request.media) {
        TryxRuntimeMediaEntry entry;
        if (!mediaModel_.uniqueEntryByName(mediaName, &entry) ||
            entry.size == 0 ||
            (entry.source != 1U && entry.source != 2U) ||
            entry.mediaId.size() != 64 ||
            entry.mediaId != entry.mediaId.toLower()) {
            return fail(tr(
                "The saved layout media is not confirmed by the current catalog"));
        }
        TryxRuntimeSavedMediaRefV1 reference;
        reference.mediaId = entry.mediaId;
        reference.name = entry.name;
        reference.size = entry.size;
        reference.source = entry.source;
        reference.readOnly = entry.readOnly;
        candidate.media.append(reference);
    }
    *layout = candidate;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool RuntimeClient::savedLayoutFromDraft(const QString &name, const QString &overwriteLayoutId, const QVariantMap &fullDraft,
    TryxRuntimeSavedLayoutV1 *layout, QString *errorMessage) const {
    TryxRuntimeSavedLayoutV2 candidate;
    return savedLayoutFromDraft(name, overwriteLayoutId, fullDraft, &candidate, errorMessage)
        && tryxSavedLayoutV2ToV1(candidate, layout);
}

bool RuntimeClient::parseSavedLayoutRevision(
    const QString &revisionDecimal, quint64 *revision) {
    if (!revision || revisionDecimal.isEmpty() ||
        (revisionDecimal.size() > 1 &&
         revisionDecimal.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : revisionDecimal) {
        if (character < QLatin1Char('0') ||
            character > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const quint64 parsed = revisionDecimal.toULongLong(&ok, 10);
    if (!ok || parsed == 0 || QString::number(parsed) != revisionDecimal) {
        return false;
    }
    *revision = parsed;
    return true;
}

void RuntimeClient::invalidateSupportSnapshotRequest(
    bool notifyFailure) {
    ++supportSnapshotAttempt_;
    if (!supportSnapshotBusy_) {
        return;
    }
    supportSnapshotBusy_ = false;
    emit supportSnapshotStateChanged();
    if (notifyFailure) {
        emit supportSnapshotFailed(tr(
            "The runtime changed while the support snapshot was being collected"));
    }
}

void RuntimeClient::invalidateDeviceMediaMetadataRequest(
    bool notifyFailure) {
    ++deviceMediaMetadataAttempt_;
    if (!deviceMediaMetadataPending_) {
        pendingDeviceMediaMetadataArtifactId_.clear();
        return;
    }
    const QString artifactId =
        pendingDeviceMediaMetadataArtifactId_;
    deviceMediaMetadataPending_ = false;
    pendingDeviceMediaMetadataArtifactId_.clear();
    if (notifyFailure) {
        emit deviceMediaMetadataFailed(
            artifactId,
            tr("The runtime changed while device media metadata was being requested"));
    }
}

bool RuntimeClient::handshakeContextIsCurrent(
    quint64 epoch, quint64 handshakeAttempt,
    const QString &owner) const {
    return serviceAvailable_ &&
           epoch == serviceEpoch_ &&
           handshakeAttempt == handshakeAttempt_ &&
           !owner.isEmpty() && owner == runtimeOwner_ &&
           currentRuntimeOwner() == owner;
}

bool RuntimeClient::dbusSignalContextIsCurrent() const {
    if (!calledFromDBus()) {
        return false;
    }
    const QDBusMessage signal = message();
    const QString owner = signal.service();
    return signal.type() == QDBusMessage::SignalMessage &&
           !owner.isEmpty() &&
           handshakeContextIsCurrent(
               serviceEpoch_, handshakeAttempt_, owner);
}

QString RuntimeClient::currentRuntimeOwner() const {
    if (!bus_.isConnected() || !bus_.interface()) {
        return {};
    }
    const QDBusReply<QString> reply =
        bus_.interface()->serviceOwner(tryxRuntimeServiceName());
    return reply.isValid() ? reply.value() : QString();
}

void RuntimeClient::clearCapabilityState() {
    const bool changed = capabilitiesReady_ ||
                         !runtimeCapabilities_.isEmpty() ||
                         deviceCapabilitiesReady_ ||
                         !deviceCapabilities_.isEmpty();
    runtimeCapabilitiesPending_ = false;
    connectionRevisionRefreshes_ = 0;
    connectionRevisionDiagnostic_.clear();
    ++displaySnapshotReadAttempt_;
    displaySnapshotReadPending_ = displaySnapshotReadAgain_ = false;
    runtimeCapabilitiesFailed_ = false;
    capabilitiesReady_ = false;
    runtimeCapabilities_.clear();
    invalidateSupportSnapshotRequest(true);
    invalidateDeviceMediaMetadataRequest(true);
    ++deviceCapabilitiesAttempt_;
    deviceCapabilitiesReady_ = false;
    deviceCapabilitiesSnapshot_ = {};
    deviceCapabilities_.clear();
    clearDeviceSpecificationsState();
    clearPresentationPreferencesState();
    clearSavedLayoutsState();
    if (changed) {
        emit capabilitiesChanged();
    }
    emit displayChanged();
}

void RuntimeClient::clearMetricsCatalogState() {
    const bool changed = metricsCatalogReady_ ||
                         !metricsCatalog_.isEmpty();
    metricsCatalogPending_ = false;
    metricsCatalogReady_ = false;
    metricsCatalogLegacyFallback_ = false;
    metricsCatalog_.clear();
    if (changed) {
        emit metricsCatalogChanged();
    }
}

void RuntimeClient::refreshLegacyMetricsCatalog() {
    if (!metricsCatalogReady_ ||
        !metricsCatalogLegacyFallback_) {
        return;
    }
    const QStringList known = tryxMetricsCatalog();
    QStringList fallback;
    const auto appendKnown = [&known, &fallback](
                                 const QStringList &metrics) {
        for (const QString &metric : metrics) {
            if (known.contains(metric) &&
                !fallback.contains(metric)) {
                fallback.append(metric);
            }
        }
    };
    appendKnown(metrics_.availableMetrics);
    appendKnown(metrics_.metrics);
    if (displayStateValid()) {
        appendKnown(display_.sysinfoLabels);
        appendKnown(display_.sysinfoLabels2);
    }
    if (metricsCatalog_ == fallback) {
        return;
    }
    metricsCatalog_ = fallback;
    emit metricsCatalogChanged();
}

void RuntimeClient::clearDeviceCapabilityState() {
    const bool changed = deviceCapabilitiesReady_ ||
                         !deviceCapabilities_.isEmpty();
    ++deviceCapabilitiesAttempt_;
    ++displaySnapshotReadAttempt_;
    displaySnapshotReadPending_ = displaySnapshotReadAgain_ = false;
    deviceCapabilitiesReady_ = false;
    deviceCapabilitiesSnapshot_ = {};
    deviceCapabilities_.clear();
    clearDeviceSpecificationsState();
    if (changed) {
        emit capabilitiesChanged();
    }
}

void RuntimeClient::clearDeviceSpecificationsState() {
    ++deviceSpecificationsAttempt_;
    deviceSpecificationsPending_ = false;
    if (runtimeCapabilitiesFailed_) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::RuntimeUnavailable);
        return;
    }
    if (!capabilitiesReady_ ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceSpecificationsV1Token())) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::NotSupported);
        return;
    }
    if (!connection_.connected) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::Disconnected);
        return;
    }
    if (!connection_.printerClassConnected) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::Unsupported);
        return;
    }
    if (!connection_.printerClassDevicePresent ||
        !runtimeCapabilities_.contains(
            tryxRuntimeDeviceCapabilitiesV1Token())) {
        setDeviceSpecificationsState(
            DeviceSpecificationsState::RuntimeUnavailable);
        return;
    }
    setDeviceSpecificationsState(
        DeviceSpecificationsState::Unavailable);
}

void RuntimeClient::setDeviceSpecificationsState(
    DeviceSpecificationsState state,
    const TryxRuntimeDeviceSpecificationsV1 &specifications) {
    const TryxRuntimeDeviceSpecificationsV1 next =
        state == DeviceSpecificationsState::Ready
        ? specifications
        : TryxRuntimeDeviceSpecificationsV1{};
    const bool changed =
        deviceSpecificationsState_ != state ||
        deviceSpecificationsSnapshot_.schemaVersion != next.schemaVersion ||
        deviceSpecificationsSnapshot_.deviceIdentity != next.deviceIdentity ||
        deviceSpecificationsSnapshot_.connectionRevision !=
            next.connectionRevision ||
        deviceSpecificationsSnapshot_.physicalGeneration !=
            next.physicalGeneration ||
        deviceSpecificationsSnapshot_.status != next.status ||
        deviceSpecificationsSnapshot_.reportedProductName !=
            next.reportedProductName ||
        deviceSpecificationsSnapshot_.videoOutputWidth !=
            next.videoOutputWidth ||
        deviceSpecificationsSnapshot_.videoOutputHeight !=
            next.videoOutputHeight ||
        deviceSpecificationsSnapshot_.screenType != next.screenType ||
        deviceSpecificationsSnapshot_.usbAutoKeepalive !=
            next.usbAutoKeepalive;
    deviceSpecificationsState_ = state;
    deviceSpecificationsSnapshot_ = next;
    if (changed) {
        emit deviceSpecificationsChanged();
    }
}

void RuntimeClient::clearRuntimeState() {
    const QString oldDisplayIdentity =
        displayDeviceIdentity();
    const bool retainLegacyDisplayPayload =
        !legacyDisplayStateIdentity_.isEmpty();
    compatible_ = false;
    apiVersion_ = 0;
    ++handshakeAttempt_;
    handshakePending_ = false;
    runtimeOwner_.clear();
    clearCapabilityState();
    clearMetricsCatalogState();
    connection_ = {};
    metrics_ = {};
    if (retainLegacyDisplayPayload) {
        display_.valid = false;
        display_.revision = 0;
    } else {
        display_ = {};
        legacyDisplayStateIdentity_.clear();
    }
    displayRevisionReceived_ = false;
    displaySnapshot_ = {};
    displaySnapshotRequired_ = false;
    ++displaySnapshotReadAttempt_;
    displaySnapshotReadPending_ = displaySnapshotReadAgain_ = false;
    displaySnapshotReadFailed_ = false;
    legacyLayoutConfirmed_ = false;
    legacyBrightnessConfirmed_ = false;
    activeOperationId_.clear();
    activeOperation_ = {};
    pendingLegacyScreenConfig_ = {};
    pendingLegacyScreenConfigValid_ = false;
    mediaModel_.clear();
    operationModel_.clear();
    emit connectionChanged();
    emit operationChanged();
    emit metricsChanged();
    emit displayChanged();
    if (oldDisplayIdentity != displayDeviceIdentity()) {
        emit displayDeviceIdentityChanged();
    }
}

void RuntimeClient::refreshConnection() {
    const quint64 epoch = serviceEpoch_, handshake = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!handshakeContextIsCurrent(epoch, handshake, owner)) return;
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetConnectionSnapshot")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch, handshake, owner]() {
                QDBusPendingReply<TryxRuntimeSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (!handshakeContextIsCurrent(epoch, handshake, owner)) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyConnectionSnapshot(reply.value());
            });
}

void RuntimeClient::refreshOperations() {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetOperations")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeOperationsSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyOperationsSnapshot(reply.value());
            });
}

void RuntimeClient::refreshMetrics() {
    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        return;
    }
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetMetricsState")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch, handshakeAttempt, owner]() {
                QDBusPendingReply<TryxRuntimeMetricsState> reply =
                    *watcher;
                watcher->deleteLater();
                if (!handshakeContextIsCurrent(
                        epoch, handshakeAttempt, owner)) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyMetricsState(reply.value());
            });
}

void RuntimeClient::refreshDisplay() {
    if (!legacyConnected() && !capabilitiesReady_) return;
    if (usesDisplaySnapshotV1()) {
        refreshDisplaySnapshotV1();
        return;
    }
    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        return;
    }
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetDisplayState")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch, handshakeAttempt, owner]() {
                QDBusPendingReply<TryxRuntimeDisplayState> reply =
                    *watcher;
                watcher->deleteLater();
                if (!handshakeContextIsCurrent(
                        epoch, handshakeAttempt, owner)) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyDisplayState(reply.value());
            });
}

void RuntimeClient::setDiagnostic(const QString &message) {
    if (diagnostic_ == message) {
        return;
    }
    diagnostic_ = message;
    emit diagnosticChanged();
}

bool RuntimeClient::mutationReady(const QString &action) {
    if (!serviceAvailable_ || !compatible_) {
        setDiagnostic(tr("%1 is unavailable because the runtime is not ready")
                          .arg(action));
    } else if (legacyConnected()) {
        if (!operationBusy()) {
            return true;
        }
        setDiagnostic(tr("%1 is blocked while another operation is active")
                          .arg(action));
    } else if (!connection_.printerClassDevicePresent) {
        setDiagnostic(tr("%1 requires a TRYX printer-class device")
                          .arg(action));
    } else if (!connection_.displaySessionActive) {
        setDiagnostic(tr(
            "%1 is blocked until the TRYX display session is active")
                          .arg(action));
    } else if (operationBusy()) {
        setDiagnostic(tr("%1 is blocked while another operation is active")
                          .arg(action));
    } else {
        return true;
    }
    emit userMessage(diagnostic_, true);
    return false;
}

bool RuntimeClient::manager1Ready(
    const QString &action, bool requireConnected) {
    if (!serviceAvailable_ || !compatible_) {
        setDiagnostic(
            tr("%1 is unavailable because the runtime is not ready")
                .arg(action));
    } else if (requireConnected &&
               !connection_.connected) {
        setDiagnostic(
            tr("%1 requires a connected TRYX device")
                .arg(action));
    } else {
        return true;
    }
    emit userMessage(diagnostic_, true);
    return false;
}

QString RuntimeClient::legacyDeviceIdentity() const {
    const QString identity =
        !connection_.serial.trimmed().isEmpty()
        ? connection_.serial.trimmed()
        : connection_.productId.trimmed();
    return identity.isEmpty()
        ? QStringLiteral("legacy")
        : QStringLiteral("legacy:%1").arg(identity);
}

QString RuntimeClient::legacyDisplayIdentity() const {
    const QString serial = connection_.serial.trimmed();
    return serial.isEmpty()
        ? QString()
        : QStringLiteral("legacy:%1").arg(serial);
}

QString RuntimeClient::nextOperationId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void RuntimeClient::sendOperation(
    const QString &method, const QVariantList &arguments,
    const QString &operationId, const QString &kind) {
    if (offline_) {
        offlineRequests_.append(
            {method, arguments, operationId, kind});
        return;
    }
    const quint64 epoch = serviceEpoch_;
    const quint64 handshakeAttempt = handshakeAttempt_;
    const QString owner = runtimeOwner_;
    if (!handshakeContextIsCurrent(
            epoch, handshakeAttempt, owner)) {
        const QString error = tr(
            "The runtime changed before the operation request was sent");
        QTimer::singleShot(
            0, this, [this, operationId, kind, error]() {
                if (displaySubmission_.active() &&
                    displaySubmission_.id == operationId &&
                    !displaySubmission_.resultEmitted) {
                    finishDisplaySubmission(
                        QStringLiteral("Failed"), error, false);
                }
                reportOperationRequestFailure(
                    operationId, kind, error, false);
            });
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), method);
    message.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, operationId, kind, epoch,
             handshakeAttempt, owner]() {
                QDBusPendingReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (!handshakeContextIsCurrent(
                        epoch, handshakeAttempt, owner)) {
                    const QString error = tr(
                        "The runtime changed before the operation request was acknowledged");
                    reportOperationRequestFailure(
                        operationId, kind, error, true);
                    return;
                }
                if (!reply.isValid() ||
                    !operationAcknowledgementMatches(
                        operationId, reply.value(), {})) {
                    const QString error = reply.isValid()
                        ? tr("The runtime returned an unexpected operation identity")
                        : reply.error().message();
                    qWarning().noquote()
                        << "TRYX operation acknowledgement requires reconciliation:"
                        << "kind=" << kind
                        << "expected=" << operationId
                        << "returned=" << reply.value()
                        << "error=" << error;
                    reconcileOperationAcknowledgement(
                        operationId, kind, error, epoch,
                        handshakeAttempt, owner,
                        reply.isValid() &&
                            !reply.value().isEmpty());
                    return;
                }
                setDiagnostic({});
                emit operationRequestAccepted(
                    operationId, kind);
                emit userMessage(
                    tr("%1 operation accepted").arg(kind), false);
                if (kind != QStringLiteral("CacheCleanup")) {
                    QTimer::singleShot(
                        0, this, &RuntimeClient::refreshOperations);
                }
            });
}

void RuntimeClient::reconcileOperationAcknowledgement(
    const QString &operationId, const QString &kind,
    const QString &initialError, quint64 epoch,
    quint64 handshakeAttempt, const QString &owner,
    bool unexpectedNonEmptyIdentity) {
    QDBusInterface runtime(
        owner, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetOperation"), operationId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, kind, initialError, epoch,
         handshakeAttempt, owner, unexpectedNonEmptyIdentity]() {
            const QDBusPendingReply<TryxRuntimeOperationInfo> reply =
                *watcher;
            watcher->deleteLater();
            if (!handshakeContextIsCurrent(
                    epoch, handshakeAttempt, owner)) {
                const QString error = tr(
                    "The runtime changed before the operation request could be reconciled");
                reportOperationRequestFailure(
                    operationId, kind, error, true);
                return;
            }
            const bool exactCleanup =
                kind != QStringLiteral("CacheCleanup") ||
                (reply.isValid() &&
                 reply.value().kind == QStringLiteral("CacheCleanup"));
            if (reply.isValid() && exactCleanup &&
                operationAcknowledgementMatches(
                    operationId, QString(), reply.value())) {
                qInfo().noquote()
                    << "TRYX operation acknowledgement reconciled:"
                    << "kind=" << kind
                    << "operation=" << operationId
                    << "state=" << reply.value().state;
                setDiagnostic({});
                emit operationRequestAccepted(
                    operationId, kind);
                emit userMessage(
                    tr("%1 operation accepted").arg(kind),
                    false);
                if (kind == QStringLiteral("CacheCleanup")) {
                    emit operationUpdated(reply.value());
                } else {
                    QTimer::singleShot(
                        0, this, &RuntimeClient::refreshOperations);
                }
                return;
            }
            const QString reconciliationError =
                reply.isValid()
                ? tr("The expected operation is absent from the runtime")
                : reply.error().message();
            const QString error = initialError.isEmpty()
                ? reconciliationError
                : initialError;
            qWarning().noquote()
                << "TRYX operation acknowledgement reconciliation failed:"
                << "kind=" << kind
                << "operation=" << operationId
                << "error=" << reconciliationError;
            const bool outcomeUnknown =
                unexpectedNonEmptyIdentity ||
                !reply.isValid() ||
                !reply.value().id.isEmpty();
            reportOperationRequestFailure(
                operationId, kind, error, outcomeUnknown);
        });
}

void RuntimeClient::reportOperationRequestFailure(
    const QString &operationId, const QString &kind,
    const QString &message, bool outcomeUnknown) {
    if (outcomeUnknown && displaySubmission_.active() &&
        displaySubmission_.id == operationId &&
        !displaySubmission_.resultEmitted) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"), message, true);
    }
    setDiagnostic(message);
    emit operationRequestFailed(
        operationId, kind, message, outcomeUnknown);
    emit operationRequestRejected(operationId, kind, message);
    emit userMessage(message, true);
}

bool RuntimeClient::operationAcknowledgementMatches(
    const QString &expectedOperationId,
    const QString &returnedOperationId,
    const TryxRuntimeOperationInfo &observedOperation) {
    if (expectedOperationId.isEmpty()) {
        return false;
    }
    return returnedOperationId == expectedOperationId ||
           observedOperation.id == expectedOperationId;
}

void RuntimeClient::sendVoidCall(
    const QString &interfaceName, const QString &method,
    const QVariantList &arguments) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        interfaceName, method);
    message.setArguments(arguments);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<> reply = *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    emit userMessage(diagnostic_, true);
                }
            });
}

void RuntimeClient::sendLegacyDisplaySubmission(
    const QString &method, const QVariantList &arguments,
    const QString &submissionId) {
    if (offline_) {
        acknowledgeLegacyDisplaySubmission(submissionId);
        offlineRequests_.append({
            method, arguments, submissionId,
            QStringLiteral("Apply"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *baselineWatcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetSnapshot")), this);
    connect(
        baselineWatcher, &QDBusPendingCallWatcher::finished, this,
        [this, baselineWatcher, epoch, submissionId,
         method, arguments]() {
            QDBusPendingReply<TryxRuntimeSnapshot> baselineReply =
                *baselineWatcher;
            baselineWatcher->deleteLater();
            if (!displaySubmission_.active() ||
                displaySubmission_.id != submissionId ||
                displaySubmission_.resultEmitted) {
                return;
            }
            if (epoch != serviceEpoch_) {
                finishLegacyDisplayPreflightFailure(
                    submissionId,
                    tr("The runtime changed before the legacy display request was dispatched"),
                    true);
                return;
            }
            if (!baselineReply.isValid()) {
                const QString error =
                    baselineReply.error().message();
                finishLegacyDisplayPreflightFailure(
                    submissionId, error, false);
                emit userMessage(error, true);
                return;
            }

            const TryxRuntimeSnapshot snapshot =
                baselineReply.value();
            const bool snapshotIsLegacy =
                snapshot.connected &&
                !snapshot.printerClassConnected &&
                !snapshot.printerClassDevicePresent;
            const QString snapshotSerial =
                snapshot.serial.trimmed();
            const QString snapshotIdentity =
                snapshotSerial.isEmpty()
                ? QString()
                : QStringLiteral("legacy:%1")
                      .arg(snapshotSerial);
            if (!snapshotIsLegacy ||
                snapshot.revision < connection_.revision ||
                snapshotIdentity !=
                    displaySubmission_.deviceIdentity) {
                finishLegacyDisplayPreflightFailure(
                    submissionId,
                    tr("The legacy display identity changed before dispatch"),
                    false);
                return;
            }

            displaySubmission_.startingDisplayRevision = qMax(
                displaySubmission_.startingDisplayRevision,
                snapshot.revision);
            applyConnectionSnapshot(snapshot);

            QDBusMessage message =
                QDBusMessage::createMethodCall(
                    tryxRuntimeServiceName(),
                    tryxRuntimeObjectPath(),
                    tryxRuntimeInterfaceName(), method);
            message.setArguments(arguments);
            auto *requestWatcher =
                new QDBusPendingCallWatcher(
                    bus_.asyncCall(
                        message, kRuntimeCallTimeoutMs),
                    this);
            connect(
                requestWatcher,
                &QDBusPendingCallWatcher::finished,
                this,
                [this, requestWatcher, epoch,
                 submissionId]() {
                    QDBusPendingReply<> reply =
                        *requestWatcher;
                    requestWatcher->deleteLater();
                    if (!displaySubmission_.active() ||
                        displaySubmission_.id != submissionId ||
                        displaySubmission_.resultEmitted) {
                        return;
                    }
                    if (epoch != serviceEpoch_) {
                        finishDisplaySubmission(
                            QStringLiteral("Unresolved"),
                            tr("The runtime changed before the legacy display request was acknowledged"),
                            true);
                        return;
                    }
                    if (!reply.isValid()) {
                        const QString error =
                            reply.error().message();
                        finishDisplaySubmission(
                            QStringLiteral("Unresolved"),
                            error, true);
                        emit userMessage(error, true);
                        return;
                    }
                    acknowledgeLegacyDisplaySubmission(
                        submissionId);
                });
        });
}

void RuntimeClient::finishLegacyDisplayPreflightFailure(
    const QString &submissionId, const QString &message,
    bool runtimeInvalidated) {
    if (!displaySubmission_.active() ||
        displaySubmission_.id != submissionId ||
        displaySubmission_.resultEmitted) {
        return;
    }
    finishDisplaySubmission(
        runtimeInvalidated
            ? QStringLiteral("Unresolved")
            : QStringLiteral("Rejected"),
        message, runtimeInvalidated);
}

void RuntimeClient::acknowledgeLegacyDisplaySubmission(
    const QString &submissionId) {
    if (!displaySubmission_.active() ||
        displaySubmission_.id != submissionId ||
        displaySubmission_.resultEmitted) {
        return;
    }
    displaySubmission_.legacyRequestAcknowledged = true;
    if (!displaySubmission_.legacyCandidateObserved) {
        return;
    }

    const TryxRuntimeDisplayState candidate =
        displaySubmission_.legacyCandidateState;
    displaySubmission_.legacyCandidateObserved = false;
    displaySubmission_.legacyCandidateState = {};
    if (candidate.revision <=
        displaySubmission_.startingDisplayRevision) {
        return;
    }
    if (candidate.deviceSerial.trimmed() !=
            displaySubmission_.deviceIdentity ||
        displayDeviceIdentity() !=
            displaySubmission_.deviceIdentity) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            tr("The legacy display identity changed before the request was confirmed"),
            true);
        return;
    }
    if (displaySubmission_.legacyScreenConfig) {
        pendingLegacyScreenConfig_ = {};
        pendingLegacyScreenConfigValid_ = false;
    }
    display_ = candidate;
    displayRevisionReceived_ = true;
    legacyDisplayStateIdentity_ =
        displaySubmission_.deviceIdentity;
    if (displaySubmission_.legacyScreenConfig) {
        legacyLayoutConfirmed_ = true;
    }
    if (displaySubmission_.legacyBrightness) {
        legacyBrightnessConfirmed_ = true;
    }
    emit displayChanged();
    observeDisplaySubmissionState(display_);
}

void RuntimeClient::sendLegacyScreenConfig(
    const TryxRuntimeApplyRequest &request) {
    pendingLegacyScreenConfig_ = request;
    pendingLegacyScreenConfigValid_ = true;
    sendVoidCall(
        tryxRuntimeInterfaceName(),
        QStringLiteral("SetScreenConfig"),
        legacyScreenConfigArguments(request));
}

QVariantList RuntimeClient::legacyScreenConfigArguments(
    const TryxRuntimeApplyRequest &request) {
    return {
        request.media,
        request.ratio,
        request.screenMode,
        request.playMode,
        request.sysinfoLabels,
        request.settingsPosition,
        request.settingsColor,
        request.settingsAlign,
        request.settingsBadges,
        request.filterOpacity,
        request.presetId,
        request.sysinfoLabels2,
        request.settingsBadges2,
        request.waterfallMode,
    };
}

bool RuntimeClient::claimLegacyUploadSource(
    const QString &operationId, const QString &sourcePath,
    QString *errorMessage) {
    if (legacyUpload_.active()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Wait for the current legacy upload to finish");
        }
        return false;
    }

    const QFileInfo info(sourcePath);
    const QString absolutePath = info.absoluteFilePath();
    const QByteArray encoded = QFile::encodeName(absolutePath);
    struct stat status {};
    if (operationId.isEmpty() || !info.exists() ||
        !info.isFile() || info.isSymLink() ||
        info.suffix().isEmpty() ||
        ::lstat(encoded.constData(), &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) !=
            (S_IRUSR | S_IWUSR) ||
        status.st_nlink != 1 || status.st_size <= 0) {
        if (errorMessage) {
            *errorMessage = tr(
                "The private legacy upload source is not a safe regular file");
        }
        return false;
    }

    const QString claimedName =
        QStringLiteral("%1-legacy-%2.%3")
            .arg(info.completeBaseName(), operationId,
                 info.suffix().toLower());
    const QString claimedPath =
        info.dir().filePath(claimedName);
    const QByteArray encodedClaim =
        QFile::encodeName(claimedPath);
    if (::link(encoded.constData(),
               encodedClaim.constData()) != 0) {
        if (errorMessage) {
            *errorMessage = tr(
                "Could not protect the legacy upload source: %1")
                                .arg(QString::fromLocal8Bit(
                                    std::strerror(errno)));
        }
        return false;
    }

    struct stat claimedStatus {};
    if (::lstat(encodedClaim.constData(),
                &claimedStatus) != 0 ||
        !S_ISREG(claimedStatus.st_mode) ||
        claimedStatus.st_dev != status.st_dev ||
        claimedStatus.st_ino != status.st_ino ||
        claimedStatus.st_nlink < 2) {
        ::unlink(encodedClaim.constData());
        if (errorMessage) {
            *errorMessage = tr(
                "The protected legacy upload source could not be verified");
        }
        return false;
    }

    legacyUpload_.operationId = operationId;
    legacyUpload_.sourcePath = absolutePath;
    legacyUpload_.claimedPath = claimedPath;
    legacyUpload_.device =
        static_cast<quint64>(status.st_dev);
    legacyUpload_.inode =
        static_cast<quint64>(status.st_ino);
    legacyUpload_.epoch = serviceEpoch_;
    legacyUpload_.rejectionEmitted = false;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

void RuntimeClient::beginLegacyUpload(
    const QString &operationId) {
    if (!legacyUpload_.active() ||
        legacyUpload_.operationId != operationId) {
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeInterfaceName(),
        QStringLiteral("UploadMedia"));
    message.setArguments({legacyUpload_.claimedPath});
    const quint64 epoch = legacyUpload_.epoch;
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    legacyUploadDeadline_.start();
    emit operationChanged();
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, epoch]() {
            const QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (!legacyUpload_.active() ||
                legacyUpload_.operationId != operationId ||
                legacyUpload_.epoch != epoch ||
                epoch != serviceEpoch_) {
                return;
            }
            if (!reply.isValid()) {
                rejectLegacyUpload(
                    reply.error().message(), true);
                return;
            }
            setDiagnostic(tr(
                "Waiting for the legacy device to finish the upload"));
            emit operationChanged();
        });
}

void RuntimeClient::finishLegacyUpload(
    const QString &filename) {
    if (!legacyUpload_.active()) {
        return;
    }
    const QString operationId =
        legacyUpload_.operationId;
    const bool alreadyRejected =
        legacyUpload_.rejectionEmitted;
    clearLegacyUpload(true, true);
    if (alreadyRejected) {
        setDiagnostic(tr(
            "The legacy upload completed after the client timeout"));
        emit userMessage(diagnostic_, false);
        return;
    }
    setDiagnostic({});
    emit operationRequestAccepted(
        operationId, QStringLiteral("Upload"));
    emit userMessage(
        filename.trimmed().isEmpty()
            ? tr("Legacy upload completed")
            : tr("Legacy upload completed: %1").arg(filename),
        false);
}

void RuntimeClient::rejectLegacyUpload(
    const QString &message, bool restoreSource) {
    if (!legacyUpload_.active()) {
        return;
    }
    const QString operationId =
        legacyUpload_.operationId;
    const bool alreadyRejected =
        legacyUpload_.rejectionEmitted;
    if (restoreSource &&
        !fileIdentityMatches(
            legacyUpload_.sourcePath,
            legacyUpload_.device,
            legacyUpload_.inode) &&
        fileIdentityMatches(
            legacyUpload_.claimedPath,
            legacyUpload_.device,
            legacyUpload_.inode)) {
        const QByteArray claimed =
            QFile::encodeName(legacyUpload_.claimedPath);
        const QByteArray source =
            QFile::encodeName(legacyUpload_.sourcePath);
        ::link(claimed.constData(), source.constData());
    }
    clearLegacyUpload(false, true);
    const QString error = message.trimmed().isEmpty()
        ? tr("Legacy upload failed")
        : message.trimmed();
    setDiagnostic(error);
    if (!alreadyRejected) {
        emit operationRequestRejected(
            operationId, QStringLiteral("Upload"), error);
        emit userMessage(error, true);
    }
}

void RuntimeClient::clearLegacyUpload(
    bool removeSource, bool removeClaim) {
    if (!legacyUpload_.active()) {
        return;
    }
    const LegacyUploadState state = legacyUpload_;
    legacyUploadDeadline_.stop();
    legacyUpload_ = {};
    if (removeSource) {
        removeFileIfIdentityMatches(
            state.sourcePath, state.device, state.inode);
    }
    if (removeClaim) {
        removeFileIfIdentityMatches(
            state.claimedPath, state.device, state.inode);
    }
    emit operationChanged();
}

bool RuntimeClient::fileIdentityMatches(
    const QString &path, quint64 device, quint64 inode) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           static_cast<quint64>(status.st_dev) == device &&
           static_cast<quint64>(status.st_ino) == inode;
}

void RuntimeClient::removeFileIfIdentityMatches(
    const QString &path, quint64 device, quint64 inode) {
    if (!fileIdentityMatches(path, device, inode)) {
        return;
    }
    const QByteArray encoded = QFile::encodeName(path);
    ::unlink(encoded.constData());
}

TryxRuntimeApplyRequest RuntimeClient::baseApplyRequest() const {
    TryxRuntimeApplyRequest request;
    request.ratio = QStringLiteral("2:1");
    request.playMode = QStringLiteral("Single");
    request.screenMode = QStringLiteral("Full Screen");
    request.settingsPosition = displayLeftPosition();
    request.settingsColor = displayLeftColor();
    request.settingsAlign = displayLeftAlignment();
    request.settingsBadges = display_.settingsBadges;
    request.waterfallMode = display_.waterfallMode;
    return request;
}

TryxRuntimeApplyRequest RuntimeClient::fullScreenApplyRequest(
    const QStringList &media, const QString &playMode,
    const QStringList &metrics,
    const QStringList &badges,
    const QString &position, const QString &color,
    const QString &alignment) const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = media;
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = playMode;
    request.sysinfoLabels = metrics;
    request.settingsBadges = badges;
    request.settingsPosition = position;
    request.settingsColor = normalizedColor(
        color, QStringLiteral("#dcdcdc"));
    request.settingsAlign = alignment;
    request.replaceOverlay = true;
    return request;
}

TryxRuntimeApplyRequest RuntimeClient::splitScreenApplyRequest(
    const QString &leftMedia, const QString &rightMedia,
    const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges,
    const QString &leftPosition,
    const QString &leftColor,
    const QString &leftAlignment,
    const QString &rightPosition,
    const QString &rightColor,
    const QString &rightAlignment) const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = {leftMedia, rightMedia};
    request.screenMode = QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    request.sysinfoLabels = leftMetrics;
    request.sysinfoLabels2 = rightMetrics;
    request.settingsBadges = leftBadges;
    request.settingsBadges2 = rightBadges;
    request.settingsPosition = leftPosition;
    request.settingsColor = normalizedColor(
        leftColor, QStringLiteral("#dcdcdc"));
    request.settingsAlign = leftAlignment;
    request.settingsPosition2 = rightPosition;
    request.settingsColor2 = normalizedColor(
        rightColor, QStringLiteral("#dcdcdc"));
    request.settingsAlign2 = rightAlignment;
    request.replaceOverlay = true;
    return request;
}

bool RuntimeClient::overlayStyleValid(
    const QString &position, const QString &color,
    const QString &alignment,
    QString *errorMessage) const {
    if (position != QStringLiteral("Top") &&
        position != QStringLiteral("Bottom")) {
        if (errorMessage) {
            *errorMessage = tr("position must be Top or Bottom");
        }
        return false;
    }
    if (!isExactRgbColor(color)) {
        if (errorMessage) {
            *errorMessage = tr("text color must use exact #RRGGBB format");
        }
        return false;
    }
    if (alignment != QStringLiteral("Left") &&
        alignment != QStringLiteral("Center") &&
        alignment != QStringLiteral("Right")) {
        if (errorMessage) {
            *errorMessage = tr(
                "alignment must be Left, Center or Right");
        }
        return false;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool RuntimeClient::metricsSelectionValid(
    const QStringList &metrics, bool allowEmpty,
    MetricArea area,
    QString *errorMessage) const {
    if ((!allowEmpty && metrics.isEmpty()) ||
        metrics.size() > 3) {
        if (errorMessage) {
            *errorMessage = allowEmpty
                ? tr("select at most three metrics")
                : tr("select between one and three metrics");
        }
        return false;
    }
    const QStringList known = tryxMetricsCatalog();
    const QStringList grandfathered =
        confirmedMetricsForArea(area);
    QSet<QString> unique;
    for (const QString &metric : metrics) {
        const bool live =
            metrics_.availableMetrics.contains(metric);
        const bool retained = grandfathered.contains(metric);
        const bool catalogConfirmed = metricsCatalogReady_ &&
            metricsCatalog_.contains(metric);
        if (metric.trimmed().isEmpty() ||
            unique.contains(metric) || !known.contains(metric) ||
            !catalogConfirmed || (!live && !retained)) {
            if (errorMessage) {
                *errorMessage =
                    tr("the metric selection contains an unavailable or duplicate value");
            }
            return false;
        }
        unique.insert(metric);
    }
    return true;
}

QStringList RuntimeClient::confirmedMetricsForArea(
    MetricArea area) const {
    if (area == MetricArea::LiveConfiguration ||
        !displayStateValid()) {
        return {};
    }
    if (area == MetricArea::Full &&
        display_.screenMode == QStringLiteral("Full Screen")) {
        return display_.sysinfoLabels;
    }
    if (display_.screenMode !=
        QStringLiteral("Screen Splitting")) {
        return {};
    }
    if (area == MetricArea::Left) {
        return display_.sysinfoLabels;
    }
    if (area == MetricArea::Right) {
        return display_.sysinfoLabels2;
    }
    return {};
}

bool RuntimeClient::badgesSelectionValid(
    const QStringList &badges,
    QString *errorMessage) const {
    if (badges.size() > 2) {
        if (errorMessage) {
            *errorMessage = tr("select at most two badges");
        }
        return false;
    }

    QSet<QString> unique;
    for (const QString &badge : badges) {
        if ((badge != QStringLiteral("CPU Badge") &&
             badge != QStringLiteral("GPU Badge")) ||
            unique.contains(badge)) {
            if (errorMessage) {
                *errorMessage =
                    tr("the badge selection contains an unsupported or duplicate value");
            }
            return false;
        }
        unique.insert(badge);
    }
    return true;
}

bool RuntimeClient::metricsConfigRequest(
    bool enabled, const QStringList &metrics,
    const QString &alignment, const QString &color,
    TryxRuntimeMetricsConfigRequest *request,
    QString *errorMessage) const {
    if (!request) {
        if (errorMessage) {
            *errorMessage =
                tr("The metrics request destination is unavailable");
        }
        return false;
    }

    TryxRuntimeMetricsConfigRequest candidate;
    candidate.enabled = enabled;
    if (!enabled) {
        candidate.metrics.clear();
        candidate.alignment =
            metrics_.alignment.isEmpty()
            ? QStringLiteral("Left")
            : metrics_.alignment;
        candidate.textColor = metrics_.textColor;
        *request = candidate;
        return true;
    }

    if (!metricsSelectionValid(
            metrics, false, MetricArea::LiveConfiguration,
            errorMessage)) {
        return false;
    }
    if (alignment != QStringLiteral("Left") &&
        alignment != QStringLiteral("Center") &&
        alignment != QStringLiteral("Right")) {
        if (errorMessage) {
            *errorMessage =
                tr("Select a supported metrics alignment");
        }
        return false;
    }
    const QColor selected(color);
    if (!selected.isValid()) {
        if (errorMessage) {
            *errorMessage =
                tr("Enter a valid metrics text color");
        }
        return false;
    }
    candidate.metrics = metrics;
    candidate.alignment = alignment;
    candidate.textColor =
        static_cast<quint32>(
            selected.rgb() & 0x00ffffff);
    *request = candidate;
    return true;
}

void RuntimeClient::applyDisplayMutation(
    const TryxRuntimeDisplayMutation &mutation) {
    if (!mutationReady(tr("Display settings"))) {
        return;
    }
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.display = mutation;
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApply"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Display"));
}

QString RuntimeClient::beginDisplaySubmission(
    const TryxRuntimeApplyRequest &request,
    bool layoutPresent, bool brightnessPresent,
    bool orientationPresent, bool legacyScreenConfig,
    bool legacyBrightness, const QString &method,
    const QString &savedLayoutId,
    quint64 savedLayoutRevision, const QVariantMap &badgeChoices) {
    if (!legacyScreenConfig && !legacyBrightness && !capabilitiesReady_) {
        setDiagnostic(tr("Apply is blocked until a coherent display snapshot is available"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    TryxRuntimeOverlayBadgesV1 normalizedBadges;
    if (!normalizeBadgeDraft(badgeChoices, request, &normalizedBadges)
        || (tryxOverlayBadgesHaveCustomText(normalizedBadges) && !customBadgeTextSupported())) {
        const QString error = tr("Custom badge text is invalid or is not supported by the current runtime and device");
        setDiagnostic(error);
        emit userMessage(error, true);
        return {};
    }
    DisplaySubmissionState submission;
    submission.id = nextOperationId();
    submission.request = request;
    submission.startingDisplayRevision =
        legacyScreenConfig || legacyBrightness
        ? qMax(display_.revision, connection_.revision)
        : displayRevision();
    submission.coherentSnapshot = !legacyScreenConfig && !legacyBrightness && usesDisplaySnapshotV1();
    if (submission.coherentSnapshot) {
        if (!displayStateValid() || !runtimeCapabilities_.contains(tryxRuntimeApplyWithBadgesV1Token())) {
            setDiagnostic(tr("Apply is blocked until a coherent display snapshot is available"));
            emit userMessage(diagnostic_, true);
            return {};
        }
        submission.expectedBadges = request.replaceOverlay ? normalizedBadges : displaySnapshot_.badges;
        submission.physicalGeneration = displaySnapshot_.physicalGeneration;
        submission.productId = connection_.productId;
        submission.owner = runtimeOwner_;
        submission.serviceEpoch = serviceEpoch_;
        submission.handshakeAttempt = handshakeAttempt_;
    }
    submission.layoutPresent = layoutPresent;
    submission.brightnessPresent = brightnessPresent;
    submission.orientationPresent = orientationPresent;
    submission.legacyScreenConfig = legacyScreenConfig;
    submission.legacyBrightness = legacyBrightness;
    if (legacyScreenConfig || legacyBrightness) {
        submission.deviceIdentity = legacyDisplayIdentity();
        if (submission.deviceIdentity.isEmpty()) {
            const QString error = tr(
                "Apply is blocked until the legacy display serial is confirmed");
            setDiagnostic(error);
            emit userMessage(error, true);
            return {};
        }
    } else {
        const QString connectionIdentity =
            connection_.serial.trimmed();
        const QString confirmedIdentity =
            display_.deviceSerial.trimmed();
        if (!display_.valid || connectionIdentity.isEmpty() ||
            confirmedIdentity.isEmpty() ||
            connectionIdentity != confirmedIdentity) {
            const QString error = tr(
                "Apply is blocked until the current display identity is confirmed");
            setDiagnostic(error);
            emit userMessage(error, true);
            return {};
        }
        submission.deviceIdentity = confirmedIdentity;
    }
    displaySubmission_ = submission;

    if (legacyScreenConfig) {
        pendingLegacyScreenConfig_ = request;
        pendingLegacyScreenConfigValid_ = true;
    }
    displayApplyDeadline_.start();
    setDiagnostic({});
    emit operationChanged();
    emit displayApplyStarted(submission.id);

    if (legacyScreenConfig) {
        sendLegacyDisplaySubmission(
            method, legacyScreenConfigArguments(request),
            submission.id);
    } else if (legacyBrightness) {
        sendLegacyDisplaySubmission(
            method, {request.display.brightness},
            submission.id);
    } else if (!savedLayoutId.isEmpty()) {
        sendOperation(
            submission.coherentSnapshot && savedLayoutsV2Supported() ? QStringLiteral("QueueSavedLayoutApplyV2") : method,
            {submission.id, savedLayoutId,
             QVariant::fromValue(savedLayoutRevision),
             submission.coherentSnapshot && savedLayoutsV2Supported() ? QVariant::fromValue(TryxRuntimeApplyWithBadgesV1{1, request, normalizedBadges}) : QVariant::fromValue(request)},
            submission.id, QStringLiteral("SavedLayoutApply"));
    } else {
        sendOperation(
            submission.coherentSnapshot ? QStringLiteral("QueueApplyWithBadgesV1") : method,
            {submission.id, submission.coherentSnapshot ? QVariant::fromValue(TryxRuntimeApplyWithBadgesV1{1, request, normalizedBadges}) : QVariant::fromValue(request)},
            submission.id, QStringLiteral("Apply"));
    }
    return submission.id;
}

bool RuntimeClient::displaySubmissionMatches(
    const TryxRuntimeDisplayState &state) const {
    const bool legacyBrightnessOnly =
        displaySubmission_.legacyBrightness &&
        !displaySubmission_.legacyScreenConfig;
    if (!displaySubmission_.active() ||
        (!state.valid && !legacyBrightnessOnly) ||
        state.revision <=
            displaySubmission_.startingDisplayRevision) {
        return false;
    }
    if (displaySubmission_.legacyScreenConfig ||
        displaySubmission_.legacyBrightness) {
        if (displaySubmission_.deviceIdentity.isEmpty() ||
            state.deviceSerial.trimmed() !=
                displaySubmission_.deviceIdentity ||
            displayDeviceIdentity() !=
                displaySubmission_.deviceIdentity) {
            return false;
        }
    } else if (displaySubmission_.deviceIdentity.isEmpty() ||
               state.deviceSerial.trimmed() !=
                   displaySubmission_.deviceIdentity) {
        return false;
    }

    const TryxRuntimeApplyRequest &request =
        displaySubmission_.request;
    const auto colorsMatch = [](const QString &left,
                                const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) == 0;
    };
    if (displaySubmission_.layoutPresent) {
        if (state.screenMode != request.screenMode ||
            state.playMode != request.playMode ||
            state.media != request.media ||
            state.sysinfoLabels != request.sysinfoLabels ||
            state.settingsBadges != request.settingsBadges ||
            state.settingsPosition != request.settingsPosition ||
            !colorsMatch(
                state.settingsColor, request.settingsColor) ||
            state.settingsAlign != request.settingsAlign) {
            return false;
        }
        if (request.screenMode ==
                QStringLiteral("Screen Splitting") &&
            (state.sysinfoLabels2 != request.sysinfoLabels2 ||
             state.settingsBadges2 != request.settingsBadges2 ||
             state.settingsPosition2 !=
                 request.settingsPosition2 ||
             !colorsMatch(
                 state.settingsColor2,
                 request.settingsColor2) ||
             state.settingsAlign2 != request.settingsAlign2)) {
            return false;
        }
        if (!displaySubmission_.orientationPresent &&
            state.waterfallMode != request.waterfallMode) {
            return false;
        }
    }
    if (displaySubmission_.brightnessPresent &&
        state.brightness != request.display.brightness) {
        return false;
    }
    if (displaySubmission_.orientationPresent &&
        (state.mirrorMode != request.display.mirrorMode ||
         state.waterfallMode !=
             request.display.waterfallMode)) {
        return false;
    }
    return true;
}

void RuntimeClient::observeDisplaySubmissionState(
    const TryxRuntimeDisplayState &state) {
    if (displaySubmission_.coherentSnapshot) return;
    const bool legacyBrightnessOnly =
        displaySubmission_.legacyBrightness &&
        !displaySubmission_.legacyScreenConfig;
    if (!displaySubmission_.active() ||
        displaySubmission_.resultEmitted ||
        (!state.valid && !legacyBrightnessOnly) ||
        state.revision <=
            displaySubmission_.startingDisplayRevision) {
        return;
    }
    if (!displaySubmissionMatches(state)) {
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            tr("The confirmed display state does not match the submitted draft"),
            true);
        return;
    }

    displaySubmission_.matchingStateObserved = true;
    if (displaySubmission_.legacyScreenConfig ||
        displaySubmission_.legacyBrightness ||
        displaySubmission_.terminalSucceeded) {
        finishDisplaySubmission(
            QStringLiteral("Succeeded"),
            tr("The display changes were confirmed"));
    }
}

void RuntimeClient::observeDisplaySubmissionOperation(
    const TryxRuntimeOperationInfo &info) {
    if (!displaySubmission_.active() ||
        displaySubmission_.resultEmitted ||
        displaySubmission_.legacyScreenConfig ||
        displaySubmission_.legacyBrightness ||
        info.id != displaySubmission_.id) {
        return;
    }
    if (displaySubmission_.coherentSnapshot && info.deviceGeneration != displaySubmission_.physicalGeneration) return;
    if (info.state == QStringLiteral("Succeeded")) {
        displaySubmission_.terminalSucceeded = true;
        if (displaySubmission_.matchingStateObserved
            && (!displaySubmission_.coherentSnapshot || displaySubmissionContextIsCurrent())) {
            finishDisplaySubmission(
                QStringLiteral("Succeeded"),
                info.message.isEmpty()
                    ? tr("The display changes were confirmed")
                    : info.message);
        }
        return;
    }
    const bool terminalFailure =
        info.state == QStringLiteral("Failed") ||
        info.state == QStringLiteral("Cancelled") ||
        info.state == QStringLiteral("RetryAvailable");
    if (terminalFailure &&
        (info.terminalOutcome ==
             QStringLiteral("PartialOrUnknown") ||
         info.errorCategory ==
             QStringLiteral("PartialOrUnknown"))) {
        const QString unresolvedMessage = info.message.isEmpty()
            ? tr("The display apply outcome is partial or unknown")
            : info.message;
        finishDisplaySubmission(
            QStringLiteral("Unresolved"),
            unresolvedMessage, true);
        return;
    }
    if (info.state == QStringLiteral("Failed") ||
        info.state == QStringLiteral("Cancelled") ||
        info.state == QStringLiteral("RetryAvailable")) {
        finishDisplaySubmission(info.state, info.message);
    }
}

void RuntimeClient::finishDisplaySubmission(
    const QString &outcome, const QString &message,
    bool unresolved) {
    if (!displaySubmission_.active() ||
        displaySubmission_.resultEmitted) {
        return;
    }
    const QString submissionId = displaySubmission_.id;
    displayApplyDeadline_.stop();
    if (unresolved) {
        if (displaySubmission_.legacyScreenConfig) {
            pendingLegacyScreenConfig_ = {};
            pendingLegacyScreenConfigValid_ = false;
        }
        displaySubmission_.legacyCandidateObserved = false;
        displaySubmission_.legacyCandidateState = {};
        displaySubmission_.unresolved = true;
        displaySubmission_.resultEmitted = true;
        if (!message.isEmpty()) {
            setDiagnostic(message);
        }
        emit operationChanged();
        emit displayApplyFinished(
            submissionId, outcome, message);
        return;
    }

    if (displaySubmission_.legacyScreenConfig) {
        pendingLegacyScreenConfig_ = {};
        pendingLegacyScreenConfigValid_ = false;
    }
    displaySubmission_ = {};
    if (outcome == QStringLiteral("Succeeded")) {
        setDiagnostic({});
    } else if (!message.isEmpty()) {
        setDiagnostic(message);
    }
    emit operationChanged();
    emit displayApplyFinished(submissionId, outcome, message);
}

void RuntimeClient::onDisplayApplyTimeout() {
    if (!displaySubmission_.active() ||
        displaySubmission_.resultEmitted) {
        return;
    }
    finishDisplaySubmission(
        QStringLiteral("Unresolved"),
        tr("The display apply result was not confirmed before the timeout"),
        true);
}

void RuntimeClient::applyOperationsSnapshot(
    const TryxRuntimeOperationsSnapshot &snapshot) {
    if (!operationModel_.applySnapshot(snapshot)) {
        return;
    }
    activeOperationId_ = snapshot.activeOperationId;
    activeOperation_ = {};
    for (const TryxRuntimeOperationInfo &info : snapshot.operations) {
        emit operationUpdated(info);
        observeDisplaySubmissionOperation(info);
        if (info.id == activeOperationId_) {
            activeOperation_ = info;
        }
    }
    emit operationChanged();
}

void RuntimeClient::applyConnectionSnapshot(
    const TryxRuntimeSnapshot &snapshot) {
    if (snapshot.revision <= connection_.revision &&
        connection_.revision != 0) {
        return;
    }
    const QString oldDisplayIdentity =
        displayDeviceIdentity();
    const bool wasConnected = connection_.connected;
    const bool wasLegacy = legacyConnected();
    const QString oldSavedIdentity =
        savedLayoutConnectionIdentity();
    const QString oldSavedProduct = connection_.productId;
    const bool oldSavedPrinterConnected =
        connection_.printerClassConnected;
    const QString oldConnectionIdentity = wasLegacy
        ? legacyDisplayIdentity()
        : connection_.serial.trimmed();
    const QString oldMediaIdentity =
        wasLegacy ? legacyDeviceIdentity()
                  : mediaModel_.deviceIdentity();
    connection_ = snapshot;
    const QString newSavedIdentity =
        savedLayoutConnectionIdentity();
    const bool savedLayoutBoundaryChanged =
        oldSavedIdentity != newSavedIdentity ||
        oldSavedProduct != connection_.productId ||
        oldSavedPrinterConnected != connection_.printerClassConnected;
    clearDeviceCapabilityState();
    if (savedLayoutBoundaryChanged) {
        clearSavedLayoutsState();
    }
    const bool isLegacy = legacyConnected();
    const QString newMediaIdentity =
        isLegacy ? legacyDeviceIdentity() : QString();
    const QString newConnectionIdentity = isLegacy
        ? legacyDisplayIdentity()
        : connection_.serial.trimmed();
    const bool displayBoundaryChanged =
        wasConnected != snapshot.connected ||
        wasLegacy != isLegacy ||
        oldConnectionIdentity != newConnectionIdentity ||
        !snapshot.connected;
    const bool retainExactLegacyDisplayState =
        !legacyDisplayStateIdentity_.isEmpty() &&
        ((!snapshot.connected &&
          !connection_.printerClassDevicePresent) ||
         (isLegacy &&
          (newConnectionIdentity.isEmpty() ||
           newConnectionIdentity ==
               legacyDisplayStateIdentity_)));
    bool displayCleared = false;
    if (displayBoundaryChanged) {
        if (displaySubmission_.active() &&
            !displaySubmission_.resultEmitted) {
            finishDisplaySubmission(
                QStringLiteral("Unresolved"),
                tr("The display identity changed before the apply result was confirmed"),
                true);
        }
        if (!retainExactLegacyDisplayState) {
            displayCleared = displayRevisionReceived_ ||
                             display_.valid ||
                             !legacyDisplayStateIdentity_.isEmpty() ||
                             legacyLayoutConfirmed_ ||
                             legacyBrightnessConfirmed_;
            display_ = {};
            displayRevisionReceived_ = false;
            legacyDisplayStateIdentity_.clear();
            legacyLayoutConfirmed_ = false;
            legacyBrightnessConfirmed_ = false;
        } else {
            display_.valid = false;
            legacyLayoutConfirmed_ = false;
            legacyBrightnessConfirmed_ = false;
        }
    }
    if (wasLegacy != isLegacy ||
        (isLegacy && oldMediaIdentity != newMediaIdentity) ||
        !snapshot.connected) {
        mediaModel_.clear();
    }
    if (isLegacy) {
        mediaModel_.applyLegacyFiles(
            snapshot.mediaFiles, snapshot.revision,
            newMediaIdentity);
    }
    refreshLegacyMetricsCatalog();
    if (!snapshot.diagnostic.isEmpty()) {
        setDiagnostic(snapshot.diagnostic);
    }
    emit connectionChanged();
    if (displayCleared) {
        emit displayChanged();
    }
    if (oldDisplayIdentity != displayDeviceIdentity()) {
        emit displayDeviceIdentityChanged();
    }
    if (displayBoundaryChanged && !isLegacy &&
        snapshot.printerClassDevicePresent) {
        QTimer::singleShot(
            0, this, &RuntimeClient::refreshDisplay);
    }
    if (capabilitiesReady_ &&
        runtimeCapabilities_.contains(
            tryxRuntimeDeviceCapabilitiesV1Token())) {
        requestDeviceCapabilities(
            serviceEpoch_, handshakeAttempt_, runtimeOwner_);
    }
    if (savedLayoutBoundaryChanged && savedLayoutsSupported()) {
        QTimer::singleShot(
            0, this, &RuntimeClient::refreshSavedLayouts);
    }
}

void RuntimeClient::applyMetricsState(
    const TryxRuntimeMetricsState &state) {
    if (state.revision <= metrics_.revision &&
        metrics_.revision != 0) {
        return;
    }
    metrics_ = state;
    refreshLegacyMetricsCatalog();
    if (!state.diagnostic.isEmpty()) {
        setDiagnostic(state.diagnostic);
    }
    emit metricsChanged();
}

void RuntimeClient::applyDisplayState(
    const TryxRuntimeDisplayState &state) {
    if (legacyConnected() || !capabilitiesReady_ || usesDisplaySnapshotV1()) {
        return;
    }
    if (state.valid) {
        const QString expectedIdentity =
            connection_.serial.trimmed();
        if (!connection_.printerClassDevicePresent ||
            expectedIdentity.isEmpty() ||
            state.deviceSerial.trimmed() != expectedIdentity) {
            if (displaySubmission_.active() &&
                !displaySubmission_.resultEmitted &&
                !displaySubmission_.legacyScreenConfig &&
                !displaySubmission_.legacyBrightness &&
                state.revision >
                    displaySubmission_.startingDisplayRevision) {
                finishDisplaySubmission(
                    QStringLiteral("Unresolved"),
                    tr("The confirmed display state belongs to a different device"),
                    true);
            }
            return;
        }
    }
    if (displayRevisionReceived_ &&
        state.revision <= display_.revision) {
        return;
    }
    const QString oldDisplayIdentity =
        displayDeviceIdentity();
    display_ = state;
    displayRevisionReceived_ = true;
    legacyDisplayStateIdentity_.clear();
    legacyLayoutConfirmed_ = false;
    legacyBrightnessConfirmed_ = false;
    refreshLegacyMetricsCatalog();
    if (!state.diagnostic.isEmpty()) {
        setDiagnostic(state.diagnostic);
    }
    emit displayChanged();
    if (oldDisplayIdentity != displayDeviceIdentity()) {
        emit displayDeviceIdentityChanged();
    }
    if (!displaySubmission_.legacyScreenConfig &&
        !displaySubmission_.legacyBrightness) {
        observeDisplaySubmissionState(state);
    }
}

void RuntimeClient::updateActiveOperation(
    const TryxRuntimeOperationInfo &info) {
    if (OperationListModel::isTerminal(info)) {
        if (activeOperationId_ == info.id) {
            activeOperationId_.clear();
            activeOperation_ = {};
        }
    } else {
        activeOperationId_ = info.id;
        activeOperation_ = info;
    }
    if (!info.message.isEmpty() &&
        info.state == QStringLiteral("Failed")) {
        setDiagnostic(info.message);
        emit userMessage(info.message, true);
    }
    emit operationChanged();
}
