#include "supportsnapshot.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QGlobalStatic>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <limits>

namespace tryx {
namespace {

constexpr qsizetype kMaximumSnapshotBytes = 256 * 1024;
constexpr qsizetype kMaximumOperations = 32;
constexpr qsizetype kMaximumEvents = 256;
constexpr qsizetype kMaximumEventFields = 16;
constexpr qsizetype kMaximumVersionLength = 128;
constexpr qint64 kMaximumCount = 1000000;
constexpr qint64 kMaximumAttempt = 1000000;
constexpr qint64 kMaximumEventNumber = 1000000000000LL;
constexpr qint64 kEventWindowMs = 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kMaximumClockSkewMs = 5LL * 60LL * 1000LL;

struct StoredSupportEvent {
    qint64 utcMs = 0;
    qint64 monotonicMs = 0;
    quint64 generation = 0;
    QString eventName;
    QJsonObject fields;
};

struct SupportEventRing {
    QMutex mutex;
    QList<StoredSupportEvent> events;
};

Q_GLOBAL_STATIC(SupportEventRing, supportEventRing)

qint64 processMonotonicMilliseconds() {
    struct ProcessClock {
        ProcessClock() { timer.start(); }
        QElapsedTimer timer;
    };
    static const ProcessClock clock;
    return clock.timer.elapsed();
}

const QSet<QString> &knownEventNames() {
    static const QSet<QString> names{
        QStringLiteral("endpoint_configured"),
        QStringLiteral("command_started"),
        QStringLiteral("command_completed"),
        QStringLiteral("readiness_probe_safely_retried"),
        QStringLiteral("readiness_failed"),
        QStringLiteral("transfer_transport_open_completed"),
        QStringLiteral("bootstrap_completed"),
        QStringLiteral("overlay_activation_started"),
        QStringLiteral("overlay_activation_completed"),
        QStringLiteral("overlay_lease_mode_selected"),
        QStringLiteral("recovery_completed"),
        QStringLiteral("endpoint_removed"),
        QStringLiteral("physical_generation_changed"),
        QStringLiteral("endpoint_discovered"),
        QStringLiteral("overlay_activation_pending"),
        QStringLiteral("transfer_transport_open_started"),
        QStringLiteral("protocol_readiness_started"),
        QStringLiteral("device_info_confirmed"),
        QStringLiteral("transfer_session_active"),
        QStringLiteral("post_bootstrap_ping_pending"),
        QStringLiteral("display_session_stopped"),
        QStringLiteral("display_session_active"),
        QStringLiteral("display_session_lost")
    };
    return names;
}

const QSet<QString> &sessionStates() {
    static const QSet<QString> values{
        QStringLiteral("passive"),
        QStringLiteral("awaiting-protocol-readiness"),
        QStringLiteral("starting"),
        QStringLiteral("awaiting-overlay-activation"),
        QStringLiteral("active"),
        QStringLiteral("recovering"),
        QStringLiteral("lost")
    };
    return values;
}

const QHash<QString, QSet<QString>> &eventEnumValues() {
    static const QHash<QString, QSet<QString>> values{
        {QStringLiteral("lease_mode"),
         {QStringLiteral("ping-and-overlay-lease"),
          QStringLiteral("ping-only")}},
        {QStringLiteral("command_class"),
         {QStringLiteral("post-bootstrap-ping"),
          QStringLiteral("overlay-lease"),
          QStringLiteral("ping"),
          QStringLiteral("device-info")}},
        {QStringLiteral("session_state"), sessionStates()},
        {QStringLiteral("state_from"), sessionStates()},
        {QStringLiteral("state_to"), sessionStates()},
        {QStringLiteral("outcome"),
         {QStringLiteral("sent"),
          QStringLiteral("retryable-failure"),
          QStringLiteral("fatal-failure"),
          QStringLiteral("failed"),
          QStringLiteral("succeeded")}},
        {QStringLiteral("failure_class"),
         {QStringLiteral("persistent-input-transport"),
          QStringLiteral("transport-open-failed"),
          QStringLiteral("bootstrap-failed"),
          QStringLiteral("device-info-not-confirmed")}},
        {QStringLiteral("config_status"),
         {QStringLiteral("test-default"),
          QStringLiteral("unreadable-or-invalid"),
          QStringLiteral("loaded"),
          QStringLiteral("default-no-config")}},
        {QStringLiteral("discovery_state"),
         {QStringLiteral("absent"),
          QStringLiteral("rockchip-gadget-391a-0006"),
          QStringLiteral("enumerating-printer-class"),
          QStringLiteral("ready"),
          QStringLiteral("permission-denied"),
          QStringLiteral("ambiguous"),
          QStringLiteral("monitoring-unavailable")}}
    };
    return values;
}

const QSet<QString> &eventBooleanFields() {
    static const QSet<QString> fields{
        QStringLiteral("overlay_present"),
        QStringLiteral("endpoint_selected")
    };
    return fields;
}

const QSet<QString> &eventNumericFields() {
    static const QSet<QString> fields{
        QStringLiteral("retry_attempt"),
        QStringLiteral("backoff_ms"),
        QStringLiteral("elapsed_ms"),
        QStringLiteral("recovery_attempt"),
        QStringLiteral("disconnect_count")
    };
    return fields;
}

bool hasUnsafeScalarCharacter(const QString &value) {
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            code == 0x061c || code == 0x200e || code == 0x200f ||
            (code >= 0x202a && code <= 0x202e) ||
            (code >= 0x2066 && code <= 0x2069)) {
            return true;
        }
    }
    return false;
}

QString sanitizedVersion(QString value) {
    value = value.trimmed();
    if (value.isEmpty() || value.size() > kMaximumVersionLength ||
        hasUnsafeScalarCharacter(value)) {
        return {};
    }
    for (const QChar character : value) {
        if (!character.isLetterOrNumber() &&
            character != QLatin1Char('.') &&
            character != QLatin1Char('-') &&
            character != QLatin1Char('_') &&
            character != QLatin1Char('+') &&
            character != QLatin1Char(' ')) {
            return {};
        }
    }
    if (value.compare(QStringLiteral("unknown"),
                      Qt::CaseInsensitive) == 0) {
        return {};
    }
    return value;
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
    if (productId == QStringLiteral("1021") ||
        productId == QStringLiteral("1011") ||
        productId == QStringLiteral("2011")) {
        return productId;
    }
    if (productId == QStringLiteral("cm01") ||
        productId == QStringLiteral("cm01_se")) {
        return QStringLiteral("cm01_se");
    }
    return {};
}

QString canonicalProductId(const QString &productId) {
    const QString code = normalizedProductCode(productId);
    if (code.isEmpty()) {
        return {};
    }
    return code == QStringLiteral("cm01_se")
        ? code
        : QStringLiteral("391a:") + code;
}

QString modelForProductId(const QString &productId) {
    const QString code = normalizedProductCode(productId);
    if (code == QStringLiteral("1021") ||
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

QString fixedToken(const QString &value,
                   const QSet<QString> &knownValues,
                   const QString &fallback = QStringLiteral("Other")) {
    return knownValues.contains(value) ? value : fallback;
}

QString operationKind(const QString &value) {
    static const QSet<QString> values{
        QStringLiteral("StageDeviceMedia"),
        QStringLiteral("ReplaceDeviceMedia"),
        QStringLiteral("RecoveredMediaUpload"),
        QStringLiteral("EnsureMediaAndApply"),
        QStringLiteral("UploadAndApply"),
        QStringLiteral("Upload"),
        QStringLiteral("DeleteMedia"),
        QStringLiteral("Apply"),
        QStringLiteral("MetricsConfig"),
        QStringLiteral("UploadRetry")
    };
    return fixedToken(value, values);
}

QString operationState(const QString &value) {
    static const QSet<QString> values{
        QStringLiteral("Preflight"),
        QStringLiteral("Pulling"),
        QStringLiteral("Refreshing"),
        QStringLiteral("Converting"),
        QStringLiteral("Hashing"),
        QStringLiteral("Uploading"),
        QStringLiteral("Applying"),
        QStringLiteral("Deleting"),
        QStringLiteral("Succeeded"),
        QStringLiteral("Failed"),
        QStringLiteral("Cancelled"),
        QStringLiteral("RetryAvailable")
    };
    return fixedToken(value, values);
}

QString operationStage(const QString &value) {
    static const QSet<QString> values{
        QStringLiteral("EnsuringSession"),
        QStringLiteral("PullingDeviceMedia"),
        QStringLiteral("Preparing"),
        QStringLiteral("HashingSource"),
        QStringLiteral("Converting"),
        QStringLiteral("Uploading"),
        QStringLiteral("RefreshingMedia"),
        QStringLiteral("ReusingExisting"),
        QStringLiteral("Applying"),
        QStringLiteral("DeletePreflight"),
        QStringLiteral("ReadingReferences"),
        QStringLiteral("ReconcilingReferences"),
        QStringLiteral("Deleting"),
        QStringLiteral("ReconcilingDelete"),
        QStringLiteral("RecoveringFinalization"),
        QStringLiteral("ReconcileOnly"),
        QStringLiteral("RetryAvailable"),
        QStringLiteral("Cancelled"),
        QStringLiteral("Rejected"),
        QStringLiteral("Succeeded"),
        QStringLiteral("Failed")
    };
    return fixedToken(value, values);
}

QString operationTerminalOutcome(const QString &value) {
    if (value.isEmpty() || value == QStringLiteral("None")) {
        return QStringLiteral("None");
    }
    static const QSet<QString> values{
        QStringLiteral("NotStarted"),
        QStringLiteral("Rejected"),
        QStringLiteral("VerificationFailed"),
        QStringLiteral("Succeeded"),
        QStringLiteral("Cancelled"),
        QStringLiteral("FinalizationUnknown"),
        QStringLiteral("PartialOrUnknown")
    };
    return fixedToken(value, values);
}

QString operationRetryMode(const QString &value) {
    if (value.isEmpty() || value == QStringLiteral("None")) {
        return QStringLiteral("None");
    }
    static const QSet<QString> values{
        QStringLiteral("PreparedMedia"),
        QStringLiteral("DeleteReconcile"),
        QStringLiteral("DeleteReconciliation"),
        QStringLiteral("ApplyVerification"),
        QStringLiteral("UploadVerified"),
        QStringLiteral("ReconcileOnly")
    };
    return fixedToken(value, values);
}

QString operationErrorCategory(const QString &value) {
    if (value.isEmpty()) {
        return QStringLiteral("None");
    }
    if (value == QStringLiteral("UserCancelled") ||
        value == QStringLiteral("Cancelled")) {
        return QStringLiteral("Cancelled");
    }
    if (value.contains(QStringLiteral("PartialOrUnknown")) ||
        value.contains(QStringLiteral("FinalizationUnknown")) ||
        value.contains(QStringLiteral("Verification"))) {
        return QStringLiteral("UnknownOutcome");
    }
    if (value.contains(QStringLiteral("Persistence")) ||
        value.contains(QStringLiteral("WriteFailed")) ||
        value.contains(QStringLiteral("CleanupFailed")) ||
        value.contains(QStringLiteral("Commit"))) {
        return QStringLiteral("Persistence");
    }
    if (value.contains(QStringLiteral("Retry")) ||
        value.contains(QStringLiteral("Recovery")) ||
        value.contains(QStringLiteral("Reconcile")) ||
        value.contains(QStringLiteral("Journal"))) {
        return QStringLiteral("Recovery");
    }
    if (value.contains(QStringLiteral("Invalid")) ||
        value.contains(QStringLiteral("Unsupported")) ||
        value.contains(QStringLiteral("NotAllowed")) ||
        value.contains(QStringLiteral("Protected")) ||
        value.contains(QStringLiteral("ReadOnly"))) {
        return QStringLiteral("Validation");
    }
    if (value.contains(QStringLiteral("Device")) ||
        value.contains(QStringLiteral("Session")) ||
        value.contains(QStringLiteral("Transport")) ||
        value.contains(QStringLiteral("Endpoint")) ||
        value.contains(QStringLiteral("FileList"))) {
        return QStringLiteral("Device");
    }
    return QStringLiteral("Other");
}

qint64 boundedCount(qsizetype value) {
    if (value <= 0) {
        return 0;
    }
    return std::min<qint64>(static_cast<qint64>(value), kMaximumCount);
}

qint64 boundedUnsigned(quint64 value) {
    return value > static_cast<quint64>(std::numeric_limits<qint64>::max())
        ? std::numeric_limits<qint64>::max()
        : static_cast<qint64>(value);
}

bool parseEventNumber(const QString &key, const QString &value,
                      qint64 *result) {
    if (!result || value.isEmpty() || value.size() > 20 ||
        value.startsWith(QLatin1Char('+')) ||
        hasUnsafeScalarCharacter(value)) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok, 10);
    const qint64 minimum = key == QStringLiteral("elapsed_ms") ? -1 : 0;
    if (!ok || parsed < minimum || parsed > kMaximumEventNumber) {
        return false;
    }
    *result = parsed;
    return true;
}

QJsonObject filteredEventFields(
    const QList<QPair<QString, QString>> &fields) {
    QJsonObject result;
    const qsizetype count =
        std::min(fields.size(), kMaximumEventFields);
    for (qsizetype index = 0; index < count; ++index) {
        const QString &key = fields.at(index).first;
        const QString &value = fields.at(index).second;
        const auto enumValues = eventEnumValues().constFind(key);
        if (enumValues != eventEnumValues().constEnd()) {
            if (enumValues->contains(value)) {
                result.insert(key, value);
            }
            continue;
        }
        if (eventBooleanFields().contains(key)) {
            if (value == QStringLiteral("true")) {
                result.insert(key, true);
            } else if (value == QStringLiteral("false")) {
                result.insert(key, false);
            }
            continue;
        }
        if (eventNumericFields().contains(key)) {
            qint64 parsed = 0;
            if (parseEventNumber(key, value, &parsed)) {
                result.insert(key, parsed);
            }
        }
    }
    return result;
}

QJsonArray supportEvents(qint64 nowUtcMs) {
    QList<StoredSupportEvent> stored;
    SupportEventRing *ring = supportEventRing();
    if (!ring) {
        return {};
    }
    {
        QMutexLocker locker(&ring->mutex);
        stored = ring->events;
    }

    QJsonArray events;
    const qint64 earliest = nowUtcMs - kEventWindowMs;
    for (const StoredSupportEvent &event : stored) {
        if (event.utcMs < earliest ||
            event.utcMs > nowUtcMs + kMaximumClockSkewMs) {
            continue;
        }
        QJsonObject object;
        object.insert(QStringLiteral("event"), event.eventName);
        object.insert(
            QStringLiteral("utc"),
            QDateTime::fromMSecsSinceEpoch(event.utcMs).toUTC()
                .toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("monotonic_ms"), event.monotonicMs);
        object.insert(QStringLiteral("generation"),
                      boundedUnsigned(event.generation));
        object.insert(QStringLiteral("fields"), event.fields);
        events.append(object);
    }
    while (events.size() > kMaximumEvents) {
        events.removeFirst();
    }
    return events;
}

QJsonObject operationObject(const TryxRuntimeOperationInfo &operation) {
    QJsonObject object;
    object.insert(QStringLiteral("kind"), operationKind(operation.kind));
    object.insert(QStringLiteral("state"), operationState(operation.state));
    object.insert(QStringLiteral("stage"), operationStage(operation.stage));
    object.insert(QStringLiteral("error_category"),
                  operationErrorCategory(operation.errorCategory));
    object.insert(QStringLiteral("terminal_outcome"),
                  operationTerminalOutcome(operation.terminalOutcome));
    object.insert(QStringLiteral("primary_error_category"),
                  operationErrorCategory(
                      operation.primaryErrorCategory));
    object.insert(QStringLiteral("retry_mode"),
                  operationRetryMode(operation.retryMode));
    object.insert(
        QStringLiteral("attempt"),
        static_cast<qint64>(
            std::min<quint32>(operation.attempt,
                              static_cast<quint32>(kMaximumAttempt))));
    object.insert(QStringLiteral("apply_after_upload"),
                  operation.applyAfterUpload);
    return object;
}

QJsonObject buildRoot(const SupportSnapshotSourceV1 &source,
                      QJsonArray operations, QJsonArray events,
                      qint64 generatedAt) {

    QJsonObject runtime;
    runtime.insert(QStringLiteral("version"),
                   sanitizedVersion(source.runtimeVersion));
    runtime.insert(QStringLiteral("api_version"),
                   static_cast<qint64>(source.runtimeApiVersion));

    QJsonObject device;
    device.insert(QStringLiteral("product_id"),
                  canonicalProductId(source.connection.productId));
    device.insert(QStringLiteral("model"),
                  modelForProductId(source.connection.productId));
    device.insert(QStringLiteral("firmware_version"),
                  sanitizedVersion(source.connection.firmware));
    device.insert(QStringLiteral("app_version"),
                  sanitizedVersion(source.connection.appVersion));
    device.insert(QStringLiteral("connected"), source.connection.connected);
    device.insert(QStringLiteral("printer_class_connected"),
                  source.connection.printerClassConnected);
    device.insert(QStringLiteral("printer_class_device_present"),
                  source.connection.printerClassDevicePresent);
    device.insert(QStringLiteral("display_session_active"),
                  source.connection.displaySessionActive);
    device.insert(QStringLiteral("connection_revision"),
                  boundedUnsigned(source.connection.revision));
    device.insert(QStringLiteral("physical_generation"),
                  boundedUnsigned(source.physicalGeneration));

    QJsonObject recovery;
    recovery.insert(QStringLiteral("recovery_required"),
                    source.recoveryRequired);
    recovery.insert(QStringLiteral("firmware_interlock_active"),
                    source.firmwareRecoveryInterlockActive);
    recovery.insert(QStringLiteral("retry_candidate_present"),
                    source.retryCandidatePresent);
    recovery.insert(QStringLiteral("retry_dispatch_present"),
                    source.retryDispatchPresent);
    recovery.insert(QStringLiteral("delete_recovery_present"),
                    source.deleteRecoveryPresent);
    recovery.insert(QStringLiteral("replace_recovery_present"),
                    source.replaceRecoveryPresent);

    QJsonObject counts;
    counts.insert(QStringLiteral("media_catalog_entry_count"),
                  boundedCount(source.mediaCatalogEntryCount));
    counts.insert(QStringLiteral("artifact_count"),
                  boundedCount(source.artifactCount));
    counts.insert(QStringLiteral("operation_count"),
                  boundedCount(source.operationCount));
    counts.insert(QStringLiteral("retry_cleanup_pending_count"),
                  boundedCount(source.retryCleanupPendingCount));

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(
        QStringLiteral("generated_at_utc"),
        QDateTime::fromMSecsSinceEpoch(generatedAt).toUTC()
            .toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("runtime"), runtime);
    root.insert(QStringLiteral("device"), device);
    root.insert(QStringLiteral("recovery"), recovery);
    root.insert(QStringLiteral("counts"), counts);
    root.insert(QStringLiteral("operations"), operations);
    root.insert(QStringLiteral("events"), events);
    return root;
}

bool hasExactKeys(const QJsonObject &object,
                  QStringList expectedKeys) {
    QStringList actual = object.keys();
    actual.sort();
    expectedKeys.sort();
    return actual == expectedKeys;
}

bool isBoundedInteger(const QJsonValue &value,
                      qint64 minimum, qint64 maximum) {
    if (!value.isDouble()) {
        return false;
    }
    const qint64 integer = value.toInteger(
        std::numeric_limits<qint64>::min());
    return integer >= minimum && integer <= maximum &&
        value.toDouble() == static_cast<double>(integer);
}

bool validateEventFields(const QJsonObject &fields) {
    if (fields.size() > kMaximumEventFields) {
        return false;
    }
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const auto enumValues = eventEnumValues().constFind(it.key());
        if (enumValues != eventEnumValues().constEnd()) {
            if (!it.value().isString() ||
                !enumValues->contains(it.value().toString())) {
                return false;
            }
            continue;
        }
        if (eventBooleanFields().contains(it.key())) {
            if (!it.value().isBool()) {
                return false;
            }
            continue;
        }
        if (eventNumericFields().contains(it.key())) {
            const qint64 minimum =
                it.key() == QStringLiteral("elapsed_ms") ? -1 : 0;
            if (!isBoundedInteger(it.value(), minimum,
                                  kMaximumEventNumber)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool failValidation(QString *errorMessage, const QString &message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

}  // namespace

qsizetype supportSnapshotMaximumBytes() {
    return kMaximumSnapshotBytes;
}

void appendSupportLifecycleEvent(
    const QString &eventName, quint64 generation,
    const QList<QPair<QString, QString>> &fields,
    qint64 utcMs, qint64 monotonicMs) {
    if (!knownEventNames().contains(eventName)) {
        return;
    }
    StoredSupportEvent event;
    event.utcMs = utcMs >= 0
        ? utcMs
        : QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    event.monotonicMs = monotonicMs >= 0
        ? monotonicMs
        : processMonotonicMilliseconds();
    event.generation = generation;
    event.eventName = eventName;
    event.fields = filteredEventFields(fields);

    SupportEventRing *ring = supportEventRing();
    if (!ring) {
        return;
    }
    QMutexLocker locker(&ring->mutex);
    ring->events.append(event);
    std::stable_sort(
        ring->events.begin(), ring->events.end(),
        [](const StoredSupportEvent &left,
           const StoredSupportEvent &right) {
            if (left.utcMs != right.utcMs) {
                return left.utcMs < right.utcMs;
            }
            return left.monotonicMs < right.monotonicMs;
        });
    while (ring->events.size() > kMaximumEvents) {
        ring->events.removeFirst();
    }
}

void clearSupportLifecycleEventsForTesting() {
    SupportEventRing *ring = supportEventRing();
    if (!ring) {
        return;
    }
    QMutexLocker locker(&ring->mutex);
    ring->events.clear();
}

QString buildSupportSnapshotV1(const SupportSnapshotSourceV1 &source) {
    QJsonArray operations;
    const qsizetype start =
        std::max<qsizetype>(0, source.operations.size() - kMaximumOperations);
    for (qsizetype index = start; index < source.operations.size(); ++index) {
        operations.append(operationObject(source.operations.at(index)));
    }

    const qint64 generatedAt = source.generatedAtUtcMs > 0
        ? source.generatedAtUtcMs
        : QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    QJsonArray events = supportEvents(generatedAt);
    QJsonObject root = buildRoot(
        source, operations, events, generatedAt);
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (bytes.size() > kMaximumSnapshotBytes && !events.isEmpty()) {
        events.removeFirst();
        root = buildRoot(source, operations, events, generatedAt);
        bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
    while (bytes.size() > kMaximumSnapshotBytes && !operations.isEmpty()) {
        operations.removeFirst();
        root = buildRoot(source, operations, events, generatedAt);
        bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
    return bytes.size() <= kMaximumSnapshotBytes
        ? QString::fromUtf8(bytes)
        : QString();
}

bool supportSnapshotV1IsValid(const QString &json,
                              QString *errorMessage) {
    const QByteArray bytes = json.toUtf8();
    if (bytes.isEmpty() || bytes.size() > kMaximumSnapshotBytes) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot size is invalid"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot JSON is invalid"));
    }
    const QJsonObject root = document.object();
    if (!hasExactKeys(
            root,
            {QStringLiteral("schema_version"),
             QStringLiteral("generated_at_utc"),
             QStringLiteral("runtime"),
             QStringLiteral("device"),
             QStringLiteral("recovery"),
             QStringLiteral("counts"),
             QStringLiteral("operations"),
             QStringLiteral("events")}) ||
        root.value(QStringLiteral("schema_version")).toInt(-1) != 1) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot schema is invalid"));
    }

    const QString generatedText =
        root.value(QStringLiteral("generated_at_utc")).toString();
    const QDateTime generated =
        QDateTime::fromString(generatedText, Qt::ISODateWithMs);
    if (!generated.isValid() || generated.timeSpec() != Qt::UTC ||
        generated.toString(Qt::ISODateWithMs) != generatedText) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot timestamp is invalid"));
    }

    const QJsonValue runtimeValue = root.value(QStringLiteral("runtime"));
    if (!runtimeValue.isObject()) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot runtime is invalid"));
    }
    const QJsonObject runtime = runtimeValue.toObject();
    if (!hasExactKeys(runtime,
                      {QStringLiteral("version"),
                       QStringLiteral("api_version")}) ||
        !runtime.value(QStringLiteral("version")).isString() ||
        sanitizedVersion(runtime.value(QStringLiteral("version")).toString()) !=
            runtime.value(QStringLiteral("version")).toString() ||
        !isBoundedInteger(runtime.value(QStringLiteral("api_version")), 8, 8)) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot runtime fields are invalid"));
    }

    const QJsonValue deviceValue = root.value(QStringLiteral("device"));
    if (!deviceValue.isObject()) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot device is invalid"));
    }
    const QJsonObject device = deviceValue.toObject();
    if (!hasExactKeys(
            device,
            {QStringLiteral("product_id"),
             QStringLiteral("model"),
             QStringLiteral("firmware_version"),
             QStringLiteral("app_version"),
             QStringLiteral("connected"),
             QStringLiteral("printer_class_connected"),
             QStringLiteral("printer_class_device_present"),
             QStringLiteral("display_session_active"),
             QStringLiteral("connection_revision"),
             QStringLiteral("physical_generation")})) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot device fields are invalid"));
    }
    for (const QString &stringKey :
         {QStringLiteral("product_id"),
          QStringLiteral("model"),
          QStringLiteral("firmware_version"),
          QStringLiteral("app_version")}) {
        if (!device.value(stringKey).isString()) {
            return failValidation(
                errorMessage,
                QStringLiteral("support snapshot device scalar type is invalid"));
        }
    }
    const QString productId =
        device.value(QStringLiteral("product_id")).toString();
    const QString model = device.value(QStringLiteral("model")).toString();
    const QString firmware =
        device.value(QStringLiteral("firmware_version")).toString();
    const QString appVersion =
        device.value(QStringLiteral("app_version")).toString();
    if (canonicalProductId(productId) != productId ||
        modelForProductId(productId) != model ||
        (!firmware.isEmpty() && sanitizedVersion(firmware) != firmware) ||
        (!appVersion.isEmpty() && sanitizedVersion(appVersion) != appVersion)) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot device scalar is invalid"));
    }
    for (const QString &booleanKey :
         {QStringLiteral("connected"),
          QStringLiteral("printer_class_connected"),
          QStringLiteral("printer_class_device_present"),
          QStringLiteral("display_session_active")}) {
        if (!device.value(booleanKey).isBool()) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot device boolean is invalid"));
        }
    }
    if (!isBoundedInteger(device.value(QStringLiteral("connection_revision")),
                          0, std::numeric_limits<qint64>::max()) ||
        !isBoundedInteger(device.value(QStringLiteral("physical_generation")),
                          0, std::numeric_limits<qint64>::max())) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot revision is invalid"));
    }

    const QJsonValue recoveryValue = root.value(QStringLiteral("recovery"));
    if (!recoveryValue.isObject()) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot recovery is invalid"));
    }
    const QJsonObject recovery = recoveryValue.toObject();
    const QStringList recoveryKeys{
        QStringLiteral("recovery_required"),
        QStringLiteral("firmware_interlock_active"),
        QStringLiteral("retry_candidate_present"),
        QStringLiteral("retry_dispatch_present"),
        QStringLiteral("delete_recovery_present"),
        QStringLiteral("replace_recovery_present")
    };
    if (!hasExactKeys(recovery, recoveryKeys)) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot recovery fields are invalid"));
    }
    for (const QString &key : recoveryKeys) {
        if (!recovery.value(key).isBool()) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot recovery boolean is invalid"));
        }
    }

    const QJsonValue countsValue = root.value(QStringLiteral("counts"));
    if (!countsValue.isObject()) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot counts are invalid"));
    }
    const QJsonObject counts = countsValue.toObject();
    const QStringList countKeys{
        QStringLiteral("media_catalog_entry_count"),
        QStringLiteral("artifact_count"),
        QStringLiteral("operation_count"),
        QStringLiteral("retry_cleanup_pending_count")
    };
    if (!hasExactKeys(counts, countKeys)) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot count fields are invalid"));
    }
    for (const QString &key : countKeys) {
        if (!isBoundedInteger(counts.value(key), 0, kMaximumCount)) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot count is invalid"));
        }
    }

    const QJsonValue operationsValue =
        root.value(QStringLiteral("operations"));
    if (!operationsValue.isArray() ||
        operationsValue.toArray().size() > kMaximumOperations) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot operations are invalid"));
    }
    static const QSet<QString> knownErrorCategories{
        QStringLiteral("None"), QStringLiteral("Cancelled"),
        QStringLiteral("UnknownOutcome"), QStringLiteral("Persistence"),
        QStringLiteral("Recovery"), QStringLiteral("Validation"),
        QStringLiteral("Device"), QStringLiteral("Other")
    };
    const QStringList operationKeys{
        QStringLiteral("kind"), QStringLiteral("state"),
        QStringLiteral("stage"), QStringLiteral("error_category"),
        QStringLiteral("terminal_outcome"),
        QStringLiteral("primary_error_category"),
        QStringLiteral("retry_mode"), QStringLiteral("attempt"),
        QStringLiteral("apply_after_upload")
    };
    for (const QJsonValue &operationValue : operationsValue.toArray()) {
        if (!operationValue.isObject()) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot operation is invalid"));
        }
        const QJsonObject operation = operationValue.toObject();
        if (!hasExactKeys(operation, operationKeys) ||
            operationKind(operation.value(QStringLiteral("kind")).toString()) !=
                operation.value(QStringLiteral("kind")).toString() ||
            operationState(operation.value(QStringLiteral("state")).toString()) !=
                operation.value(QStringLiteral("state")).toString() ||
            operationStage(operation.value(QStringLiteral("stage")).toString()) !=
                operation.value(QStringLiteral("stage")).toString() ||
            !knownErrorCategories.contains(
                operation.value(QStringLiteral("error_category")).toString()) ||
            !knownErrorCategories.contains(
                operation.value(QStringLiteral("primary_error_category")).toString()) ||
            operationTerminalOutcome(
                operation.value(QStringLiteral("terminal_outcome")).toString()) !=
                operation.value(QStringLiteral("terminal_outcome")).toString() ||
            operationRetryMode(
                operation.value(QStringLiteral("retry_mode")).toString()) !=
                operation.value(QStringLiteral("retry_mode")).toString() ||
            !isBoundedInteger(operation.value(QStringLiteral("attempt")),
                              0, kMaximumAttempt) ||
            !operation.value(QStringLiteral("apply_after_upload")).isBool()) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot operation fields are invalid"));
        }
    }

    const QJsonValue eventsValue = root.value(QStringLiteral("events"));
    if (!eventsValue.isArray() || eventsValue.toArray().size() > kMaximumEvents) {
        return failValidation(errorMessage,
                              QStringLiteral("support snapshot events are invalid"));
    }
    const QStringList eventKeys{
        QStringLiteral("event"), QStringLiteral("utc"),
        QStringLiteral("monotonic_ms"), QStringLiteral("generation"),
        QStringLiteral("fields")
    };
    const qint64 generatedMs = generated.toMSecsSinceEpoch();
    for (const QJsonValue &eventValue : eventsValue.toArray()) {
        if (!eventValue.isObject()) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot event is invalid"));
        }
        const QJsonObject event = eventValue.toObject();
        const QString eventName = event.value(QStringLiteral("event")).toString();
        const QString utcText = event.value(QStringLiteral("utc")).toString();
        const QDateTime utc = QDateTime::fromString(utcText, Qt::ISODateWithMs);
        if (!hasExactKeys(event, eventKeys) ||
            !knownEventNames().contains(eventName) ||
            !utc.isValid() || utc.timeSpec() != Qt::UTC ||
            utc.toString(Qt::ISODateWithMs) != utcText ||
            utc.toMSecsSinceEpoch() < generatedMs - kEventWindowMs ||
            utc.toMSecsSinceEpoch() > generatedMs + kMaximumClockSkewMs ||
            !isBoundedInteger(event.value(QStringLiteral("monotonic_ms")),
                              0, kMaximumEventNumber) ||
            !isBoundedInteger(event.value(QStringLiteral("generation")),
                              0, std::numeric_limits<qint64>::max()) ||
            !event.value(QStringLiteral("fields")).isObject() ||
            !validateEventFields(
                event.value(QStringLiteral("fields")).toObject())) {
            return failValidation(errorMessage,
                                  QStringLiteral("support snapshot event fields are invalid"));
        }
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

}  // namespace tryx
