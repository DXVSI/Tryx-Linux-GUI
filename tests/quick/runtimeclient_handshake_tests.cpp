#include <QtTest>

#include "runtimeclient.h"
#include "supportbundlecontroller.h"
#include "supportsnapshot.h"

#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <utility>

namespace {

TryxRuntimeSnapshot connectedSnapshot(
    quint64 revision = 1,
    const QString &identity = QStringLiteral("device-a")) {
    TryxRuntimeSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.connected = true;
    snapshot.printerClassConnected = true;
    snapshot.printerClassDevicePresent = true;
    snapshot.displaySessionActive = true;
    snapshot.productId = QStringLiteral("1021");
    snapshot.serial = identity;
    return snapshot;
}

QString savedLayoutMediaId(
    const QString &deviceIdentity, const QString &name,
    quint64 size, quint32 source) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    hash.addData(deviceIdentity.toUtf8());
    hash.addData(separator);
    hash.addData(name.toUtf8());
    hash.addData(separator);
    hash.addData(QByteArray::number(size));
    hash.addData(separator);
    hash.addData(QByteArray::number(source));
    return QString::fromLatin1(hash.result().toHex());
}

TryxRuntimeApplyRequest savedLayoutRequest(
    const QString &mediaName = QStringLiteral("demo.mp4")) {
    TryxRuntimeApplyRequest request;
    request.media = {mediaName};
    request.ratio = QStringLiteral("2:1");
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Single");
    request.settingsPosition = QStringLiteral("Top");
    request.settingsColor = QStringLiteral("#ffffff");
    request.settingsAlign = QStringLiteral("Left");
    request.replaceOverlay = true;
    request.display.brightnessPresent = true;
    request.display.brightness = 50;
    request.display.orientationPresent = true;
    request.display.backlightEnabled = true;
    return request;
}

TryxRuntimeSavedLayoutV1 savedLayout(
    const QString &layoutId, const QString &name, quint64 revision,
    const QString &deviceIdentity = QStringLiteral("device-a"),
    const QString &productId = QStringLiteral("391a:1021"),
    const QString &mediaName = QStringLiteral("demo.mp4")) {
    TryxRuntimeSavedLayoutV1 layout;
    layout.layoutId = layoutId;
    layout.revision = revision;
    layout.deviceIdentity = deviceIdentity;
    layout.productId = productId;
    layout.name = name;
    layout.request = savedLayoutRequest(mediaName);

    TryxRuntimeSavedMediaRefV1 media;
    media.mediaId = savedLayoutMediaId(
        deviceIdentity, mediaName, 1024, 1);
    media.name = mediaName;
    media.size = 1024;
    media.source = 1;
    layout.media = {media};
    return layout;
}

TryxRuntimeSavedLayoutsSnapshotV1 readySavedLayoutsSnapshot(
    const QList<TryxRuntimeSavedLayoutV1> &layouts,
    quint64 revision,
    const QString &deviceIdentity = QStringLiteral("device-a"),
    const QString &productId = QStringLiteral("391a:1021")) {
    TryxRuntimeSavedLayoutsSnapshotV1 snapshot;
    snapshot.revision = revision;
    snapshot.status = QStringLiteral("Ready");
    snapshot.deviceIdentity = deviceIdentity;
    snapshot.productId = productId;
    snapshot.layouts = layouts;
    return snapshot;
}

TryxRuntimeMediaCatalogSnapshot savedLayoutMediaCatalog(
    const QString &deviceIdentity = QStringLiteral("device-a"),
    const QString &mediaName = QStringLiteral("demo.mp4"),
    quint64 revision = 1) {
    TryxRuntimeMediaEntry entry;
    entry.name = mediaName;
    entry.size = 1024;
    entry.source = 1;
    entry.mediaId = savedLayoutMediaId(
        deviceIdentity, mediaName, entry.size, entry.source);

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.deviceIdentity = deviceIdentity;
    snapshot.entries = {entry};
    return snapshot;
}

QVariantMap savedLayoutFullDraft(
    const QString &mediaName = QStringLiteral("demo.mp4")) {
    const QVariantMap layout = {
        {QStringLiteral("split"), false},
        {QStringLiteral("media"), QStringList{mediaName}},
        {QStringLiteral("playMode"), QStringLiteral("Single")},
        {QStringLiteral("metrics"), QStringList{}},
        {QStringLiteral("badges"), QStringList{}},
        {QStringLiteral("position"), QStringLiteral("Top")},
        {QStringLiteral("color"), QStringLiteral("#ffffff")},
        {QStringLiteral("alignment"), QStringLiteral("Left")},
    };
    const QVariantMap orientation = {
        {QStringLiteral("mirror"), false},
        {QStringLiteral("waterfall"), false},
    };
    return {
        {QStringLiteral("layout"), layout},
        {QStringLiteral("brightness"), 50},
        {QStringLiteral("orientation"), orientation},
    };
}

TryxRuntimeDeviceMediaArtifact claimedArtifact() {
    TryxRuntimeDeviceMediaArtifact artifact;
    artifact.schemaVersion = 1;
    artifact.operationId = QStringLiteral(
        "11111111-1111-4111-8111-111111111111");
    artifact.artifactId = QStringLiteral(
        "22222222-2222-4222-8222-222222222222");
    artifact.mediaId = QString(64, QLatin1Char('b'));
    artifact.deviceIdentity = QStringLiteral("device-a");
    artifact.remoteName =
        QStringLiteral("device-video.mp4.h264_2240x1080");
    artifact.size = 4096;
    artifact.decodedSha256 = QString(64, QLatin1Char('a'));
    artifact.localPath = QStringLiteral("/tmp/tryx-artifact-a.h264");
    artifact.logicalType = QStringLiteral("Video");
    artifact.leaseId = QStringLiteral("lease-a");
    artifact.leaseExpiresUtcMs =
        QDateTime::currentMSecsSinceEpoch() + 120000;
    return artifact;
}

TryxRuntimeDeviceMediaMetadataV1 readyDeviceMediaMetadata() {
    TryxRuntimeDeviceMediaMetadataV1 metadata;
    metadata.schemaVersion = 1;
    metadata.operationId = QStringLiteral(
        "11111111-1111-4111-8111-111111111111");
    metadata.artifactId = QStringLiteral(
        "22222222-2222-4222-8222-222222222222");
    metadata.mediaId = QString(64, QLatin1Char('b'));
    metadata.deviceIdentity = QStringLiteral("device-a");
    metadata.decodedSha256 = QString(64, QLatin1Char('a'));
    metadata.deviceGeneration = 7;
    metadata.status = QStringLiteral("Ready");
    metadata.availableFields = 0x7U;
    metadata.width = 2240;
    metadata.height = 1080;
    metadata.durationMilliseconds = 60000;
    metadata.frameRateNumerator = 30;
    metadata.frameRateDenominator = 1;
    return metadata;
}

QString validSupportSnapshot() {
    tryx::SupportSnapshotSourceV1 source;
    source.runtimeVersion = QStringLiteral("2.2.0");
    source.runtimeApiVersion = 8;
    source.connection = connectedSnapshot();
    source.physicalGeneration = 7;
    return tryx::buildSupportSnapshotV1(source);
}

class LegacyRuntimeObject final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")

public:
    TryxRuntimeSnapshot connection = connectedSnapshot();
    TryxRuntimeMetricsState metrics;
    TryxRuntimeDisplayState display;

public slots:
    quint32 GetRuntimeApiVersion() {
        return tryxRuntimeApiVersion();
    }

    TryxRuntimeSnapshot GetConnectionSnapshot() {
        return connection;
    }

    TryxRuntimeOperationsSnapshot GetOperations() {
        return {};
    }

    TryxRuntimeMediaCatalogSnapshot GetMediaCatalog() {
        return {};
    }

    TryxRuntimeMetricsState GetMetricsState() {
        return metrics;
    }

    TryxRuntimeDisplayState GetDisplayState() {
        return display;
    }
};

class CapabilityRuntimeObject final
    : public QObject,
      protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")

public:
    enum class ApiReplyMode {
        Normal,
        Delayed,
    };

    enum class RuntimeReplyMode {
        Normal,
        AccessDenied,
        UnknownInterface,
        InvalidSignature,
        Delayed,
    };

    enum class DeviceReplyMode {
        Normal,
        Delayed,
    };

    enum class DeviceSpecificationsReplyMode {
        Normal,
        Error,
        UnknownMethod,
        Delayed,
    };

    enum class MetricsCatalogReplyMode {
        Normal,
        Error,
        Delayed,
    };

    enum class PresentationReplyMode {
        Normal,
        Error,
        Delayed,
    };

    enum class SupportReplyMode {
        Normal,
        Delayed,
    };

    enum class DeviceMediaMetadataReplyMode {
        Normal,
        Error,
        UnknownMethod,
        Delayed,
    };

    enum class SavedLayoutsReplyMode {
        Normal,
        Error,
        UnknownMethod,
        Delayed,
    };

    enum class CacheCleanupQueueReplyMode {
        Normal,
        NoReply,
        Delayed,
    };

    TryxRuntimeSnapshot connection = connectedSnapshot();
    TryxRuntimeMediaCatalogSnapshot mediaCatalog;
    TryxRuntimeMetricsState metrics;
    TryxRuntimeDisplayState display;
    QStringList metricsCatalog = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("GPU Temperature"),
    };
    QStringList runtimeCapabilities = {
        tryxRuntimeDeviceCapabilitiesV1Token(),
    };
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilities = {
        1,
        QStringLiteral("device-a"),
        1,
        7,
        {
            tryxDeviceMediaUploadV1Token(),
            tryxDeviceMediaCatalogV1Token(),
        },
    };
    TryxRuntimeDeviceSpecificationsV1 deviceSpecifications = {
        1,
        QStringLiteral("device-a"),
        1,
        7,
        QStringLiteral("Ready"),
        QStringLiteral("PASE"),
        2240,
        1080,
        QStringLiteral("OLED"),
        false,
    };
    TryxRuntimePresentationPreferencesV1 presentationPreferences;
    TryxRuntimeDeviceMediaArtifact artifact = claimedArtifact();
    TryxRuntimeDeviceMediaMetadataV1 deviceMediaMetadata =
        readyDeviceMediaMetadata();
    TryxRuntimeSavedLayoutsSnapshotV1 savedLayouts;
    ApiReplyMode apiReplyMode = ApiReplyMode::Normal;
    RuntimeReplyMode runtimeReplyMode = RuntimeReplyMode::Normal;
    DeviceReplyMode deviceReplyMode = DeviceReplyMode::Normal;
    DeviceSpecificationsReplyMode deviceSpecificationsReplyMode =
        DeviceSpecificationsReplyMode::Normal;
    MetricsCatalogReplyMode metricsCatalogReplyMode =
        MetricsCatalogReplyMode::Normal;
    QDBusError::ErrorType metricsCatalogError =
        QDBusError::AccessDenied;
    QString metricsCatalogErrorMessage =
        QStringLiteral("metrics catalog denied");
    PresentationReplyMode presentationGetterReplyMode =
        PresentationReplyMode::Normal;
    PresentationReplyMode presentationSetterReplyMode =
        PresentationReplyMode::Normal;
    SupportReplyMode supportReplyMode = SupportReplyMode::Normal;
    DeviceMediaMetadataReplyMode deviceMediaMetadataReplyMode =
        DeviceMediaMetadataReplyMode::Normal;
    SavedLayoutsReplyMode savedLayoutsGetterReplyMode =
        SavedLayoutsReplyMode::Normal;
    SavedLayoutsReplyMode savedLayoutsPutReplyMode =
        SavedLayoutsReplyMode::Normal;
    SavedLayoutsReplyMode savedLayoutsDeleteReplyMode =
        SavedLayoutsReplyMode::Normal;
    CacheCleanupQueueReplyMode cacheCleanupQueueReplyMode =
        CacheCleanupQueueReplyMode::Normal;
    bool cacheCleanupCancelNoReply = false;
    QString savedLayoutsGetterErrorName = QStringLiteral(
        "org.tryx.Panorama.Error.SavedLayoutsUnavailable");
    QString savedLayoutsGetterErrorMessage =
        QStringLiteral("saved layouts unavailable");
    QString savedLayoutsPutErrorName = QStringLiteral(
        "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict");
    QString savedLayoutsPutErrorMessage =
        QStringLiteral("saved layouts revision conflict");
    QString savedLayoutsDeleteErrorName = QStringLiteral(
        "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict");
    QString savedLayoutsDeleteErrorMessage =
        QStringLiteral("saved layouts revision conflict");
    QString supportSnapshot = validSupportSnapshot();
    std::atomic_int apiCalls{0};
    std::atomic_int connectionSnapshotCalls{0};
    std::atomic_int runtimeCapabilityCalls{0};
    std::atomic_int metricsCatalogCalls{0};
    std::atomic_int deviceCapabilityCalls{0};
    std::atomic_int deviceSpecificationsCalls{0};
    std::atomic_int presentationGetterCalls{0};
    std::atomic_int presentationSetterCalls{0};
    std::atomic_int supportSnapshotCalls{0};
    std::atomic_int artifactClaimCalls{0};
    std::atomic_int deviceMediaMetadataCalls{0};
    std::atomic_int savedLayoutsGetterCalls{0};
    std::atomic_int savedLayoutsPutCalls{0};
    std::atomic_int savedLayoutsDeleteCalls{0};
    std::atomic_int savedLayoutsQueueCalls{0};
    std::atomic_int cacheCleanupQueueCalls{0};
    std::atomic_int cacheCleanupGetCalls{0};
    std::atomic_int cacheCleanupCancelCalls{0};
    TryxRuntimeOperationInfo cacheCleanupOperation;
    quint64 lastSavedLayoutsPutExpectedRevision = 0;
    TryxRuntimeSavedLayoutV1 lastSavedLayoutsPutLayout;
    quint64 lastSavedLayoutsDeleteExpectedRevision = 0;
    QString lastSavedLayoutsDeleteLayoutId;
    QDBusMessage delayedApiMessage;
    QDBusMessage delayedRuntimeMessage;
    QDBusMessage delayedDeviceMessage;
    QDBusMessage delayedDeviceSpecificationsMessage;
    QDBusMessage delayedMetricsCatalogMessage;
    QDBusMessage delayedPresentationGetterMessage;
    QDBusMessage delayedPresentationSetterMessage;
    QDBusMessage delayedSupportSnapshotMessage;
    QDBusMessage delayedDeviceMediaMetadataMessage;
    QDBusMessage delayedSavedLayoutsGetterMessage;
    QDBusMessage delayedSavedLayoutsPutMessage;
    QDBusMessage delayedSavedLayoutsDeleteMessage;
    QDBusMessage delayedCacheCleanupQueueMessage;

    bool sendDelayedApiReply(
        const QDBusConnection &connection,
        quint32 apiVersion) {
        if (delayedApiMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedApiMessage.createReply(
                QVariantList{QVariant::fromValue(apiVersion)}));
        delayedApiMessage = {};
        return sent;
    }

    bool sendDelayedRuntimeReply(
        const QDBusConnection &connection,
        const QStringList &capabilities) {
        if (delayedRuntimeMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedRuntimeMessage.createReply(
                QVariantList{QVariant::fromValue(capabilities)}));
        delayedRuntimeMessage = {};
        return sent;
    }

    bool sendDelayedDeviceReply(
        const QDBusConnection &connection,
        const TryxRuntimeDeviceCapabilitiesV1 &capabilities) {
        if (delayedDeviceMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedDeviceMessage.createReply(
                QVariantList{QVariant::fromValue(capabilities)}));
        delayedDeviceMessage = {};
        return sent;
    }

    bool sendDelayedDeviceSpecificationsReply(
        const QDBusConnection &connection,
        const TryxRuntimeDeviceSpecificationsV1 &specifications) {
        if (delayedDeviceSpecificationsMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedDeviceSpecificationsMessage.createReply(
                QVariantList{QVariant::fromValue(specifications)}));
        delayedDeviceSpecificationsMessage = {};
        return sent;
    }

    bool sendDelayedMetricsCatalogReply(
        const QDBusConnection &connection,
        const QStringList &catalog) {
        if (delayedMetricsCatalogMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedMetricsCatalogMessage.createReply(
                QVariantList{QVariant::fromValue(catalog)}));
        delayedMetricsCatalogMessage = {};
        return sent;
    }

    bool sendDelayedPresentationGetterReply(
        const QDBusConnection &connection,
        const TryxRuntimePresentationPreferencesV1 &preferences) {
        if (delayedPresentationGetterMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedPresentationGetterMessage.createReply(
                QVariantList{QVariant::fromValue(preferences)}));
        delayedPresentationGetterMessage = {};
        return sent;
    }

    bool sendDelayedPresentationSetterReply(
        const QDBusConnection &connection,
        const TryxRuntimePresentationPreferencesV1 &preferences) {
        if (delayedPresentationSetterMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedPresentationSetterMessage.createReply(
                QVariantList{QVariant::fromValue(preferences)}));
        delayedPresentationSetterMessage = {};
        return sent;
    }

    bool sendDelayedSupportSnapshotReply(
        const QDBusConnection &connection,
        const QString &snapshot) {
        if (delayedSupportSnapshotMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedSupportSnapshotMessage.createReply(
                QVariantList{QVariant::fromValue(snapshot)}));
        delayedSupportSnapshotMessage = {};
        return sent;
    }

    bool sendDelayedDeviceMediaMetadataReply(
        const QDBusConnection &connection,
        const TryxRuntimeDeviceMediaMetadataV1 &metadata) {
        if (delayedDeviceMediaMetadataMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedDeviceMediaMetadataMessage.createReply(
                QVariantList{QVariant::fromValue(metadata)}));
        delayedDeviceMediaMetadataMessage = {};
        return sent;
    }

    bool sendDelayedSavedLayoutsGetterReply(
        const QDBusConnection &connection,
        const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot) {
        if (delayedSavedLayoutsGetterMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedSavedLayoutsGetterMessage.createReply(
                QVariantList{QVariant::fromValue(snapshot)}));
        delayedSavedLayoutsGetterMessage = {};
        return sent;
    }

    bool sendDelayedSavedLayoutsPutReply(
        const QDBusConnection &connection,
        const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot) {
        if (delayedSavedLayoutsPutMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedSavedLayoutsPutMessage.createReply(
                QVariantList{QVariant::fromValue(snapshot)}));
        delayedSavedLayoutsPutMessage = {};
        return sent;
    }

    bool sendDelayedSavedLayoutsDeleteReply(
        const QDBusConnection &connection,
        const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot) {
        if (delayedSavedLayoutsDeleteMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedSavedLayoutsDeleteMessage.createReply(
                QVariantList{QVariant::fromValue(snapshot)}));
        delayedSavedLayoutsDeleteMessage = {};
        return sent;
    }

    bool sendDelayedCacheCleanupQueueReply(
        const QDBusConnection &connection) {
        if (delayedCacheCleanupQueueMessage.type() !=
            QDBusMessage::MethodCallMessage) {
            return false;
        }
        const bool sent = connection.send(
            delayedCacheCleanupQueueMessage.createReply(
                QVariantList{cacheCleanupOperation.id}));
        delayedCacheCleanupQueueMessage = {};
        return sent;
    }

public slots:
    quint32 GetRuntimeApiVersion() {
        if (apiReplyMode == ApiReplyMode::Delayed) {
            setDelayedReply(true);
            delayedApiMessage = message();
            apiCalls.fetch_add(1, std::memory_order_release);
            return 0;
        }
        apiCalls.fetch_add(1, std::memory_order_release);
        return tryxRuntimeApiVersion();
    }

    TryxRuntimeSnapshot GetConnectionSnapshot() {
        connectionSnapshotCalls.fetch_add(
            1, std::memory_order_release);
        return connection;
    }

    TryxRuntimeOperationsSnapshot GetOperations() {
        return {};
    }

    TryxRuntimeOperationInfo GetOperation(
        const QString &operationId) {
        cacheCleanupGetCalls.fetch_add(
            1, std::memory_order_release);
        return operationId == cacheCleanupOperation.id
            ? cacheCleanupOperation
            : TryxRuntimeOperationInfo{};
    }

    TryxRuntimeMediaCatalogSnapshot GetMediaCatalog() {
        return mediaCatalog;
    }

    TryxRuntimeMetricsState GetMetricsState() {
        return metrics;
    }

    TryxRuntimeDisplayState GetDisplayState() {
        return display;
    }

    QStringList GetMetricsCapabilities() {
        metricsCatalogCalls.fetch_add(
            1, std::memory_order_release);
        if (metricsCatalogReplyMode ==
            MetricsCatalogReplyMode::Error) {
            sendErrorReply(
                metricsCatalogError,
                metricsCatalogErrorMessage);
            return {};
        }
        if (metricsCatalogReplyMode ==
            MetricsCatalogReplyMode::Delayed) {
            setDelayedReply(true);
            delayedMetricsCatalogMessage = message();
            return {};
        }
        return metricsCatalog;
    }

    QStringList GetRuntimeCapabilities() {
        if (runtimeReplyMode == RuntimeReplyMode::AccessDenied) {
            sendErrorReply(
                QDBusError::AccessDenied,
                QStringLiteral("capability access denied"));
            runtimeCapabilityCalls.fetch_add(
                1, std::memory_order_release);
            return {};
        }
        if (runtimeReplyMode == RuntimeReplyMode::UnknownInterface) {
            sendErrorReply(
                QDBusError::UnknownInterface,
                QStringLiteral("capability interface unavailable"));
            runtimeCapabilityCalls.fetch_add(
                1, std::memory_order_release);
            return {};
        }
        if (runtimeReplyMode == RuntimeReplyMode::InvalidSignature) {
            sendErrorReply(
                QDBusError::InvalidSignature,
                QStringLiteral("invalid capability signature"));
            runtimeCapabilityCalls.fetch_add(
                1, std::memory_order_release);
            return {};
        }
        if (runtimeReplyMode == RuntimeReplyMode::Delayed) {
            setDelayedReply(true);
            delayedRuntimeMessage = message();
            runtimeCapabilityCalls.fetch_add(
                1, std::memory_order_release);
            return {};
        }
        runtimeCapabilityCalls.fetch_add(
            1, std::memory_order_release);
        return runtimeCapabilities;
    }

    TryxRuntimeDeviceCapabilitiesV1 GetDeviceCapabilitiesV1() {
        if (deviceReplyMode == DeviceReplyMode::Delayed) {
            setDelayedReply(true);
            delayedDeviceMessage = message();
            deviceCapabilityCalls.fetch_add(
                1, std::memory_order_release);
            return {};
        }
        deviceCapabilityCalls.fetch_add(
            1, std::memory_order_release);
        return deviceCapabilities;
    }

    TryxRuntimeDeviceSpecificationsV1 GetDeviceSpecificationsV1() {
        deviceSpecificationsCalls.fetch_add(
            1, std::memory_order_release);
        if (deviceSpecificationsReplyMode ==
            DeviceSpecificationsReplyMode::Error) {
            sendErrorReply(
                QDBusError::AccessDenied,
                QStringLiteral("device specifications denied"));
            return {};
        }
        if (deviceSpecificationsReplyMode ==
            DeviceSpecificationsReplyMode::UnknownMethod) {
            sendErrorReply(
                QDBusError::UnknownMethod,
                QStringLiteral("device specifications method missing"));
            return {};
        }
        if (deviceSpecificationsReplyMode ==
            DeviceSpecificationsReplyMode::Delayed) {
            setDelayedReply(true);
            delayedDeviceSpecificationsMessage = message();
            return {};
        }
        return deviceSpecifications;
    }

    TryxRuntimeDeviceMediaArtifact ClaimDeviceMediaArtifact(
        const QString &operationId, const QString &artifactId) {
        artifactClaimCalls.fetch_add(1, std::memory_order_release);
        if (operationId != artifact.operationId ||
            artifactId != artifact.artifactId) {
            sendErrorReply(
                QDBusError::InvalidArgs,
                QStringLiteral("unexpected artifact claim identity"));
            return {};
        }
        return artifact;
    }

    TryxRuntimeDeviceMediaMetadataV1 GetDeviceMediaMetadataV1(
        const QString &artifactId, const QString &leaseId) {
        deviceMediaMetadataCalls.fetch_add(
            1, std::memory_order_release);
        if (artifactId != artifact.artifactId ||
            leaseId != artifact.leaseId) {
            sendErrorReply(
                QDBusError::InvalidArgs,
                QStringLiteral("unexpected metadata artifact identity"));
            return {};
        }
        if (deviceMediaMetadataReplyMode ==
            DeviceMediaMetadataReplyMode::Error) {
            sendErrorReply(
                QDBusError::AccessDenied,
                QStringLiteral("device media metadata denied"));
            return {};
        }
        if (deviceMediaMetadataReplyMode ==
            DeviceMediaMetadataReplyMode::UnknownMethod) {
            sendErrorReply(
                QDBusError::UnknownMethod,
                QStringLiteral("device media metadata method missing"));
            return {};
        }
        if (deviceMediaMetadataReplyMode ==
            DeviceMediaMetadataReplyMode::Delayed) {
            setDelayedReply(true);
            delayedDeviceMediaMetadataMessage = message();
            return {};
        }
        return deviceMediaMetadata;
    }

    TryxRuntimeSavedLayoutsSnapshotV1 GetSavedLayoutsV1() {
        savedLayoutsGetterCalls.fetch_add(
            1, std::memory_order_release);
        if (savedLayoutsGetterReplyMode ==
            SavedLayoutsReplyMode::UnknownMethod) {
            sendErrorReply(
                QDBusError::UnknownMethod,
                QStringLiteral("saved layouts method missing"));
            return {};
        }
        if (savedLayoutsGetterReplyMode ==
            SavedLayoutsReplyMode::Error) {
            sendErrorReply(
                savedLayoutsGetterErrorName,
                savedLayoutsGetterErrorMessage);
            return {};
        }
        if (savedLayoutsGetterReplyMode ==
            SavedLayoutsReplyMode::Delayed) {
            setDelayedReply(true);
            delayedSavedLayoutsGetterMessage = message();
            return {};
        }
        return savedLayouts;
    }

    TryxRuntimeSavedLayoutsSnapshotV1 PutSavedLayoutV1(
        quint64 expectedSnapshotRevision,
        const TryxRuntimeSavedLayoutV1 &layout) {
        savedLayoutsPutCalls.fetch_add(
            1, std::memory_order_release);
        lastSavedLayoutsPutExpectedRevision = expectedSnapshotRevision;
        lastSavedLayoutsPutLayout = layout;
        if (savedLayoutsPutReplyMode ==
            SavedLayoutsReplyMode::UnknownMethod) {
            sendErrorReply(
                QDBusError::UnknownMethod,
                QStringLiteral("saved layout put method missing"));
            return {};
        }
        if (savedLayoutsPutReplyMode ==
            SavedLayoutsReplyMode::Error) {
            sendErrorReply(
                savedLayoutsPutErrorName,
                savedLayoutsPutErrorMessage);
            return {};
        }
        if (savedLayoutsPutReplyMode ==
            SavedLayoutsReplyMode::Delayed) {
            setDelayedReply(true);
            delayedSavedLayoutsPutMessage = message();
            return {};
        }
        return savedLayouts;
    }

    TryxRuntimeSavedLayoutsSnapshotV1 DeleteSavedLayoutV1(
        quint64 expectedSnapshotRevision, const QString &layoutId) {
        savedLayoutsDeleteCalls.fetch_add(
            1, std::memory_order_release);
        lastSavedLayoutsDeleteExpectedRevision =
            expectedSnapshotRevision;
        lastSavedLayoutsDeleteLayoutId = layoutId;
        if (savedLayoutsDeleteReplyMode ==
            SavedLayoutsReplyMode::UnknownMethod) {
            sendErrorReply(
                QDBusError::UnknownMethod,
                QStringLiteral("saved layout delete method missing"));
            return {};
        }
        if (savedLayoutsDeleteReplyMode ==
            SavedLayoutsReplyMode::Error) {
            sendErrorReply(
                savedLayoutsDeleteErrorName,
                savedLayoutsDeleteErrorMessage);
            return {};
        }
        if (savedLayoutsDeleteReplyMode ==
            SavedLayoutsReplyMode::Delayed) {
            setDelayedReply(true);
            delayedSavedLayoutsDeleteMessage = message();
            return {};
        }
        return savedLayouts;
    }

    QString QueueSavedLayoutApplyV1(
        const QString &operationId, const QString &, quint64,
        const TryxRuntimeApplyRequest &) {
        savedLayoutsQueueCalls.fetch_add(
            1, std::memory_order_release);
        return operationId;
    }

    QString QueueCacheCleanupV1(const QString &operationId) {
        cacheCleanupQueueCalls.fetch_add(
            1, std::memory_order_release);
        cacheCleanupOperation = {};
        cacheCleanupOperation.id = operationId;
        cacheCleanupOperation.kind = QStringLiteral("CacheCleanup");
        cacheCleanupOperation.state = QStringLiteral("Running");
        cacheCleanupOperation.stage =
            QStringLiteral("CleaningCatalog");
        if (cacheCleanupQueueReplyMode ==
            CacheCleanupQueueReplyMode::NoReply) {
            sendErrorReply(
                QStringLiteral("org.freedesktop.DBus.Error.NoReply"),
                QStringLiteral("cleanup reply was lost"));
            return {};
        }
        if (cacheCleanupQueueReplyMode ==
            CacheCleanupQueueReplyMode::Delayed) {
            setDelayedReply(true);
            delayedCacheCleanupQueueMessage = message();
            return {};
        }
        return operationId;
    }

    void CancelOperation(const QString &operationId) {
        cacheCleanupCancelCalls.fetch_add(
            1, std::memory_order_release);
        if (operationId == cacheCleanupOperation.id) {
            cacheCleanupOperation.state =
                QStringLiteral("Cancelled");
            cacheCleanupOperation.stage =
                QStringLiteral("Cancelled");
            cacheCleanupOperation.terminalOutcome =
                QStringLiteral("Cancelled");
            cacheCleanupOperation.errorCategory =
                QStringLiteral("UserCancelled");
        }
        if (cacheCleanupCancelNoReply) {
            sendErrorReply(
                QStringLiteral("org.freedesktop.DBus.Error.NoReply"),
                QStringLiteral("cancel reply was lost"));
        }
    }

    TryxRuntimePresentationPreferencesV1
    GetPresentationPreferencesV1() {
        presentationGetterCalls.fetch_add(
            1, std::memory_order_release);
        if (presentationGetterReplyMode ==
            PresentationReplyMode::Error) {
            sendErrorReply(
                QDBusError::Failed,
                QStringLiteral("presentation getter failed"));
            return {};
        }
        if (presentationGetterReplyMode ==
            PresentationReplyMode::Delayed) {
            setDelayedReply(true);
            delayedPresentationGetterMessage = message();
            return {};
        }
        return presentationPreferences;
    }

    TryxRuntimePresentationPreferencesV1
    SetPresentationPreferencesV1(
        quint64 expectedRevision, const QString &temperatureUnit,
        const QString &timeFormat) {
        presentationSetterCalls.fetch_add(
            1, std::memory_order_release);
        if (presentationSetterReplyMode ==
            PresentationReplyMode::Error) {
            sendErrorReply(
                QDBusError::Failed,
                QStringLiteral("presentation setter failed"));
            return {};
        }
        if (presentationSetterReplyMode ==
            PresentationReplyMode::Delayed) {
            setDelayedReply(true);
            delayedPresentationSetterMessage = message();
            return {};
        }
        if (expectedRevision != presentationPreferences.revision) {
            sendErrorReply(
                QDBusError::Failed,
                QStringLiteral("presentation revision conflict"));
            return {};
        }
        presentationPreferences.temperatureUnit = temperatureUnit;
        presentationPreferences.timeFormat = timeFormat;
        ++presentationPreferences.revision;
        return presentationPreferences;
    }

    QString GetSupportSnapshotV1() {
        supportSnapshotCalls.fetch_add(1, std::memory_order_release);
        if (supportReplyMode == SupportReplyMode::Delayed) {
            setDelayedReply(true);
            delayedSupportSnapshotMessage = message();
            return {};
        }
        return supportSnapshot;
    }
};

void configureSavedLayoutsRuntime(
    CapabilityRuntimeObject *runtime,
    const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot,
    quint64 connectionRevision = 1) {
    Q_ASSERT(runtime);
    runtime->connection = connectedSnapshot(
        connectionRevision, snapshot.deviceIdentity);
    runtime->connection.productId = snapshot.productId;
    runtime->runtimeCapabilities = {
        tryxRuntimeSavedLayoutsV1Token(),
    };
    runtime->savedLayouts = snapshot;
    runtime->mediaCatalog = savedLayoutMediaCatalog(
        snapshot.deviceIdentity, QStringLiteral("demo.mp4"),
        connectionRevision);
}

class ScopedRuntimeService final {
public:
    explicit ScopedRuntimeService(QObject *object)
        : object_(object),
          connectionName_(QStringLiteral("tryx-handshake-test-%1")
                              .arg(QUuid::createUuid().toString(
                                  QUuid::WithoutBraces))),
          connection_(QDBusConnection::connectToBus(
              QDBusConnection::SessionBus, connectionName_)) {
        worker_.start();
        object_->moveToThread(&worker_);
    }

    ~ScopedRuntimeService() {
        stop();
        QThread *applicationThread =
            QCoreApplication::instance()->thread();
        QMetaObject::invokeMethod(
            object_,
            [object = object_, applicationThread]() {
                object->moveToThread(applicationThread);
            },
            Qt::BlockingQueuedConnection);
        worker_.quit();
        worker_.wait();
        QDBusConnection::disconnectFromBus(connectionName_);
    }

    bool start() {
        if (!connection_.isConnected()) {
            return false;
        }
        objectRegistered_ = connection_.registerObject(
            tryxRuntimeObjectPath(), object_,
            QDBusConnection::ExportAllSlots);
        if (!objectRegistered_) {
            return false;
        }
        serviceRegistered_ = connection_.registerService(
            tryxRuntimeServiceName());
        return serviceRegistered_;
    }

    bool releaseServiceName() {
        if (!serviceRegistered_) {
            return true;
        }
        const bool released = connection_.unregisterService(
            tryxRuntimeServiceName());
        if (released) {
            serviceRegistered_ = false;
        }
        return released;
    }

    void stop() {
        releaseServiceName();
        if (objectRegistered_) {
            connection_.unregisterObject(tryxRuntimeObjectPath());
            objectRegistered_ = false;
        }
    }

    const QDBusConnection &connection() const {
        return connection_;
    }

    bool sendPrinterOperationsCancelled(quint64 revision) {
        QDBusMessage signal = QDBusMessage::createSignal(
            tryxRuntimeObjectPath(), tryxRuntimeInterfaceName(),
            QStringLiteral("PrinterOperationsCancelled"));
        signal.setArguments(
            QVariantList{QVariant::fromValue(revision)});
        return connection_.send(signal);
    }

    bool sendDisplaySessionChanged(bool active, quint64 revision) {
        QDBusMessage signal = QDBusMessage::createSignal(
            tryxRuntimeObjectPath(), tryxRuntimeInterfaceName(),
            QStringLiteral("DisplaySessionChanged"));
        signal.setArguments(
            QVariantList{QVariant::fromValue(active),
                         QVariant::fromValue(revision)});
        return connection_.send(signal);
    }

    bool sendPresentationPreferencesChanged(
        const TryxRuntimePresentationPreferencesV1 &preferences) {
        QDBusMessage signal = QDBusMessage::createSignal(
            tryxRuntimeObjectPath(),
            tryxRuntimeOperationsInterfaceName(),
            QStringLiteral("PresentationPreferencesChangedV1"));
        signal.setArguments(
            QVariantList{QVariant::fromValue(preferences)});
        return connection_.send(signal);
    }

    bool sendMetricsStateUpdated(
        const TryxRuntimeMetricsState &state) {
        QDBusMessage signal = QDBusMessage::createSignal(
            tryxRuntimeObjectPath(),
            tryxRuntimeOperationsInterfaceName(),
            QStringLiteral("MetricsStateUpdated"));
        signal.setArguments(
            QVariantList{QVariant::fromValue(state)});
        return connection_.send(signal);
    }

    bool sendDisplayStateUpdated(
        const TryxRuntimeDisplayState &state) {
        QDBusMessage signal = QDBusMessage::createSignal(
            tryxRuntimeObjectPath(),
            tryxRuntimeOperationsInterfaceName(),
            QStringLiteral("DisplayStateUpdated"));
        signal.setArguments(
            QVariantList{QVariant::fromValue(state)});
        return connection_.send(signal);
    }

    template <typename Function>
    bool invoke(Function &&function) {
        return QMetaObject::invokeMethod(
            object_, std::forward<Function>(function),
            Qt::BlockingQueuedConnection);
    }

private:
    QObject *object_ = nullptr;
    QString connectionName_;
    QDBusConnection connection_;
    QThread worker_;
    bool objectRegistered_ = false;
    bool serviceRegistered_ = false;
};

void processEventsFor(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 10);
    }
}

}  // namespace

class RuntimeClientHandshakeTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void fullMetricsCatalogUsesExactUniqueOwner();
    void metricsCatalogIsSeparateFromLiveAvailability();
    void missingMetricsCatalogMethodUsesBoundedFallback();
    void onlyExactUnknownMethodUsesBoundedFallback();
    void malformedMetricsCatalogFailsClosed_data();
    void malformedMetricsCatalogFailsClosed();
    void nonUnknownMetricsCatalogErrorFailsClosed_data();
    void nonUnknownMetricsCatalogErrorFailsClosed();
    void staleMetricsCatalogReplyFromSupersededAttemptSameOwnerIsDiscarded();
    void staleMetricsCatalogReplyFromPreviousOwnerIsDiscarded();
    void currentOwnerMetricsAndDisplaySignalsAreAccepted();
    void legacyApi8KeepsBaselineAndUsesEmptyCapabilities();
    void nonLegacyCapabilityErrorFailsClosed_data();
    void nonLegacyCapabilityErrorFailsClosed();
    void validCapabilitiesAreFilteredAndDeviceBound();
    void deviceSpecificationsAreCapabilityGatedAndDeviceBound();
    void deviceSpecificationsContextStatesAreExact();
    void deviceSpecificationsGetterErrorsFailClosed_data();
    void deviceSpecificationsGetterErrorsFailClosed();
    void malformedDeviceSpecificationsFailClosed_data();
    void malformedDeviceSpecificationsFailClosed();
    void displaySessionRefreshTransitionsSpecificationsToReady();
    void staleDeviceSpecificationsReplyAfterRevisionChangeIsDiscarded();
    void printerOperationsCancellationFencesDeviceSpecifications();
    void staleDeviceSpecificationsReplyFromPreviousOwnerIsDiscarded();
    void invalidDeviceCapabilitiesStayFailClosed_data();
    void invalidDeviceCapabilitiesStayFailClosed();
    void connectionRevisionInvalidatesAndRequeriesDeviceCapabilities();
    void generationOnlyCancellationInvalidatesAndRequeriesCapabilities();
    void repeatedRegistrationDuringPendingApiHandshakeRestartsHandshake();
    void staleDeviceReplyAfterRevisionChangeIsDiscarded();
    void staleReplyFromPreviousOwnerIsDiscarded();
    void presentationPreferencesAreConfirmedAndWritable();
    void invalidPresentationPreferencesFailClosed();
    void presentationSignalBeforeGetterReplyWins();
    void presentationSetterFailureKeepsConfirmedState();
    void presentationSetterFailureWaitsForReconciliation();
    void stalePresentationSignalCallbackFromPreviousOwnerIsDiscarded();
    void stalePresentationSetterReplyFromPreviousOwnerIsDiscarded();
    void supportSnapshotIsCapabilityGatedAndValidated();
    void staleSupportSnapshotReplyFromPreviousOwnerIsDiscarded();
    void offlineDeviceMediaMetadataRequestRecordsExactArtifactLease();
    void deviceMediaMetadataIsCapabilityGatedAndArtifactBound();
    void malformedDeviceMediaMetadataFailsClosed_data();
    void malformedDeviceMediaMetadataFailsClosed();
    void deviceMediaMetadataFailureDoesNotInvalidateArtifactClaim();
    void newArtifactSupersedesPendingDeviceMediaMetadataRequest();
    void staleDeviceMediaMetadataReplyFromPreviousOwnerIsDiscarded();
    void savedLayoutsRequireExactCapabilityAndConfirmedSnapshot();
    void advertisedSavedLayoutsUnknownMethodFailsClosed();
    void malformedSavedLayoutsSnapshotFailsClosed_data();
    void malformedSavedLayoutsSnapshotFailsClosed();
    void staleSavedLayoutsReplyFromPreviousOwnerIsDiscarded();
    void staleSavedLayoutPutReplyFromPreviousOwnerIsDiscarded();
    void deviceIdentityChangeInvalidatesAndRequeriesSavedLayoutsWithoutMutation();
    void nonCanonicalConnectionIdentityCannotAuthorizeOrRetainReadySavedLayouts_data();
    void nonCanonicalConnectionIdentityCannotAuthorizeOrRetainReadySavedLayouts();
    void savedLayoutPutAndDeletePublishOnlyConfirmedSnapshots();
    void savedLayoutMutationRepliesRequireExactNextRevision_data();
    void savedLayoutMutationRepliesRequireExactNextRevision();
    void savedLayoutReadsRejectSameRevisionChangesAndRollback();
    void savedLayoutConflictReconcilesAndCommitUnknownFailsClosed();
    void savedLayoutMutationsRequireCurrentOwnerBeforeSend();
    void savedLayoutTransportAmbiguityFailsClosedAndReconciles();
    void savedLayoutPreSendOwnerFailureIsDeliveredAsynchronously();
    void cacheCleanupNoReplyReconcilesOnceAndCancelDoesNotReplay();
    void cacheCleanupOwnerReplacementNeverTargetsNewOwner();
    void supportBundleControllerExportsUnavailableHostReport();
    void supportBundleControllerExportsFullAndHostOnlyReports();
    void supportBundleControllerAllowsOnlyOneInflightRequest();
    void supportBundleControllerRejectsInvalidSnapshotWithoutFile();
};

void RuntimeClientHandshakeTests::
    supportBundleControllerExportsUnavailableHostReport() {
    RuntimeClient client;
    SupportBundleController controller(&client);
    QVERIFY(!client.serviceAvailable());
    QVERIFY(!client.capabilitiesReady());
    QVERIFY(!controller.runtimeDetailsAvailable());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    controller.exportToFolder(QUrl::fromLocalFile(directory.path()));
    QCOMPARE(controller.state(), QStringLiteral("saved"));
    QVERIFY(!controller.busy());
    QFile reportFile(controller.lastExportPath());
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    const QJsonObject report =
        QJsonDocument::fromJson(reportFile.readAll()).object();
    QCOMPARE(report.value(QStringLiteral("runtime_snapshot"))
                 .toObject()
                 .value(QStringLiteral("status")).toString(),
             QStringLiteral("unavailable"));

    QTemporaryDir unsafeDirectory;
    QVERIFY(unsafeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        unsafeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::WriteGroup));
    controller.exportToFolder(
        QUrl::fromLocalFile(unsafeDirectory.path()));
    QCOMPARE(controller.state(), QStringLiteral("error"));
    QVERIFY(controller.message().contains(
        QStringLiteral("only you"), Qt::CaseInsensitive));
    QVERIFY(controller.lastExportPath().isEmpty());
}

void RuntimeClientHandshakeTests::
    supportBundleControllerExportsFullAndHostOnlyReports() {
    LegacyRuntimeObject legacyRuntime;
    ScopedRuntimeService legacyService(&legacyRuntime);
    QVERIFY(legacyService.start());

    RuntimeClient client;
    SupportBundleController controller(&client);
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QVERIFY(!controller.runtimeDetailsAvailable());

    QTemporaryDir hostDirectory;
    QVERIFY(hostDirectory.isValid());
    controller.exportToFolder(
        QUrl::fromLocalFile(hostDirectory.path()));
    QCOMPARE(controller.state(), QStringLiteral("saved"));
    QVERIFY(!controller.busy());
    QVERIFY(!controller.lastExportPath().isEmpty());
    QFile hostFile(controller.lastExportPath());
    QVERIFY(hostFile.open(QIODevice::ReadOnly));
    const QJsonObject hostReport =
        QJsonDocument::fromJson(hostFile.readAll()).object();
    QCOMPARE(hostReport.value(QStringLiteral("runtime_snapshot"))
                 .toObject()
                 .value(QStringLiteral("status")).toString(),
             QStringLiteral("unsupported"));

    QVERIFY(legacyService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject supportedRuntime;
    supportedRuntime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    ScopedRuntimeService supportedService(&supportedRuntime);
    QVERIFY(supportedService.start());
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.runtimeDetailsAvailable(), 3000);

    QTemporaryDir fullDirectory;
    QVERIFY(fullDirectory.isValid());
    controller.exportToFolder(
        QUrl::fromLocalFile(fullDirectory.path()));
    QVERIFY(controller.busy());
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(), QStringLiteral("saved"), 3000);
    QVERIFY(!controller.busy());
    QCOMPARE(supportedRuntime.supportSnapshotCalls.load(), 1);
    QFile fullFile(controller.lastExportPath());
    QVERIFY(fullFile.open(QIODevice::ReadOnly));
    const QJsonObject fullReport =
        QJsonDocument::fromJson(fullFile.readAll()).object();
    QCOMPARE(fullReport.value(QStringLiteral("runtime_snapshot"))
                 .toObject()
                 .value(QStringLiteral("status")).toString(),
             QStringLiteral("available"));
}

void RuntimeClientHandshakeTests::
    supportBundleControllerAllowsOnlyOneInflightRequest() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    runtime.supportReplyMode =
        CapabilityRuntimeObject::SupportReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    SupportBundleController controller(&client);
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.runtimeDetailsAvailable(), 3000);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QUrl folder = QUrl::fromLocalFile(directory.path());
    controller.exportToFolder(folder);
    controller.exportToFolder(folder);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.supportSnapshotCalls.load(), 1, 3000);
    QVERIFY(controller.busy());

    QVERIFY(service.releaseServiceName());
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(), QStringLiteral("error"), 3000);
    QVERIFY(!controller.busy());
    QVERIFY(controller.message().contains(
        QStringLiteral("collect"), Qt::CaseInsensitive));
    QVERIFY(controller.lastExportPath().isEmpty());
    QCOMPARE(QDir(directory.path()).entryList(
                 QStringList{QStringLiteral("*.json")},
                 QDir::Files).size(), 0);
}

void RuntimeClientHandshakeTests::
    supportBundleControllerRejectsInvalidSnapshotWithoutFile() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    runtime.supportSnapshot = QString(
        tryx::supportSnapshotMaximumBytes() + 1,
        QLatin1Char('x'));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    SupportBundleController controller(&client);
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.runtimeDetailsAvailable(), 3000);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    controller.exportToFolder(
        QUrl::fromLocalFile(directory.path()));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(), QStringLiteral("error"), 3000);
    QVERIFY(!controller.busy());
    QVERIFY(controller.lastExportPath().isEmpty());
    QCOMPARE(QDir(directory.path()).entryList(
                 QStringList{QStringLiteral("*.json")},
                 QDir::Files).size(), 0);
}

void RuntimeClientHandshakeTests::
    supportSnapshotIsCapabilityGatedAndValidated() {
    CapabilityRuntimeObject runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QVERIFY(!client.supportSnapshotAvailable());
    QVERIFY(!client.requestSupportSnapshot());
    QCOMPARE(runtime.supportSnapshotCalls.load(), 0);

    QVERIFY(service.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject supportedRuntime;
    supportedRuntime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    ScopedRuntimeService supportedService(&supportedRuntime);
    QVERIFY(supportedService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.supportSnapshotAvailable(), 3000);

    QSignalSpy ready(&client, &RuntimeClient::supportSnapshotReady);
    QSignalSpy failed(&client, &RuntimeClient::supportSnapshotFailed);
    QVERIFY(client.requestSupportSnapshot());
    QVERIFY(client.supportSnapshotBusy());
    QVERIFY(!client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(
        supportedRuntime.supportSnapshotCalls.load(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    QVERIFY(!client.supportSnapshotBusy());
    QCOMPARE(failed.count(), 0);
    QCOMPARE(ready.first().first().toString(),
             supportedRuntime.supportSnapshot);

    QVERIFY(supportedService.invoke([&supportedRuntime]() {
        supportedRuntime.supportSnapshot =
            QStringLiteral("{\"schema_version\":1,\"secret\":true}");
    }));
    QVERIFY(client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QCOMPARE(ready.count(), 1);
    QVERIFY(!client.supportSnapshotBusy());

    QVERIFY(supportedService.invoke([&supportedRuntime]() {
        supportedRuntime.supportSnapshot = QString(
            tryx::supportSnapshotMaximumBytes() + 1,
            QLatin1Char('x'));
    }));
    QVERIFY(client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 3000);
    QCOMPARE(ready.count(), 1);
    QVERIFY(!client.supportSnapshotBusy());

    QVERIFY(supportedService.invoke([&supportedRuntime]() {
        supportedRuntime.supportSnapshot = validSupportSnapshot();
        supportedRuntime.supportSnapshot.replace(
            QStringLiteral("\"version\":\"2.2.0\""),
            QStringLiteral(
                "\"version\":{\"secret\":\"/home/alice/duplicate\"},"
                "\"version\":\"2.2.0\""));
    }));
    QVERIFY(client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 3000);
    QVERIFY(!ready.last().first().toString().contains(
        QStringLiteral("/home/alice/duplicate")));
}

void RuntimeClientHandshakeTests::
    staleSupportSnapshotReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    firstRuntime.supportReplyMode =
        CapabilityRuntimeObject::SupportReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.supportSnapshotAvailable(), 3000);
    QSignalSpy ready(&client, &RuntimeClient::supportSnapshotReady);
    QSignalSpy failed(&client, &RuntimeClient::supportSnapshotFailed);
    QVERIFY(client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.supportSnapshotCalls.load(), 1, 3000);
    QVERIFY(client.supportSnapshotBusy());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);
    QVERIFY(!client.supportSnapshotBusy());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeCapabilities.append(
        tryxRuntimeSupportSnapshotV1Token());
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.supportSnapshotAvailable(), 3000);

    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedSupportSnapshotReply(
                    firstService.connection(),
                    firstRuntime.supportSnapshot);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 1);

    QVERIFY(client.requestSupportSnapshot());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    QCOMPARE(ready.first().first().toString(),
             secondRuntime.supportSnapshot);
}

void RuntimeClientHandshakeTests::
    offlineDeviceMediaMetadataRequestRecordsExactArtifactLease() {
    RuntimeClient client(true);
    const TryxRuntimeDeviceMediaArtifact artifact = claimedArtifact();

    QVERIFY(!client.requestDeviceMediaMetadata(artifact));
    QVERIFY(client.offlineRequests_.isEmpty());

    client.capabilitiesReady_ = true;
    client.runtimeCapabilities_.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    TryxRuntimeDeviceMediaArtifact invalidArtifact = artifact;
    invalidArtifact.operationId = QStringLiteral("not-a-uuid");
    QVERIFY(!client.requestDeviceMediaMetadata(invalidArtifact));
    invalidArtifact = artifact;
    invalidArtifact.leaseId.clear();
    QVERIFY(!client.requestDeviceMediaMetadata(invalidArtifact));
    QVERIFY(client.offlineRequests_.isEmpty());

    QVERIFY(client.requestDeviceMediaMetadata(artifact));
    QCOMPARE(client.deviceMediaMetadataAttempt_, quint64(1));
    QVERIFY(client.deviceMediaMetadataPending_);
    QCOMPARE(client.pendingDeviceMediaMetadataArtifactId_,
             artifact.artifactId);
    QCOMPARE(client.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest &request =
        client.offlineRequests_.constFirst();
    QCOMPARE(request.method,
             QStringLiteral("GetDeviceMediaMetadataV1"));
    QCOMPARE(request.arguments,
             QVariantList({artifact.artifactId, artifact.leaseId}));
    QCOMPARE(request.operationId, artifact.operationId);
    QCOMPARE(request.kind,
             QStringLiteral("GetDeviceMediaMetadataV1"));
}

void RuntimeClientHandshakeTests::
    deviceMediaMetadataIsCapabilityGatedAndArtifactBound() {
    {
        CapabilityRuntimeObject unsupportedRuntime;
        ScopedRuntimeService unsupportedService(&unsupportedRuntime);
        QVERIFY(unsupportedService.start());

        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
        QSignalSpy ready(
            &client, &RuntimeClient::deviceMediaMetadataReady);
        QSignalSpy failed(
            &client, &RuntimeClient::deviceMediaMetadataFailed);
        QVERIFY(ready.isValid());
        QVERIFY(failed.isValid());

        QVERIFY(!client.requestDeviceMediaMetadata(
            unsupportedRuntime.artifact));
        processEventsFor(100);
        QCOMPARE(
            unsupportedRuntime.deviceMediaMetadataCalls.load(
                std::memory_order_acquire),
            0);
        QCOMPARE(ready.count(), 0);
    }

    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    runtime.deviceCapabilities.physicalGeneration = 8;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QSignalSpy ready(
        &client, &RuntimeClient::deviceMediaMetadataReady);
    QSignalSpy failed(
        &client, &RuntimeClient::deviceMediaMetadataFailed);
    QVERIFY(ready.isValid());
    QVERIFY(failed.isValid());

    QVERIFY(client.requestDeviceMediaMetadata(runtime.artifact));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(
        runtime.deviceMediaMetadataCalls.load(
            std::memory_order_acquire),
        1);
    const TryxRuntimeDeviceMediaMetadataV1 metadata =
        qvariant_cast<TryxRuntimeDeviceMediaMetadataV1>(
            ready.constFirst().constFirst());
    QCOMPARE(metadata.operationId, runtime.artifact.operationId);
    QCOMPARE(metadata.artifactId, runtime.artifact.artifactId);
    QCOMPARE(metadata.mediaId, runtime.artifact.mediaId);
    QCOMPARE(metadata.deviceIdentity, runtime.artifact.deviceIdentity);
    QCOMPARE(metadata.decodedSha256, runtime.artifact.decodedSha256);
    QCOMPARE(metadata.deviceGeneration, quint64(7));
    QCOMPARE(metadata.status, QStringLiteral("Ready"));
    QCOMPARE(metadata.availableFields, quint32(0x7));
    QCOMPARE(metadata.width, quint32(2240));
    QCOMPARE(metadata.height, quint32(1080));
    QCOMPARE(metadata.durationMilliseconds, quint64(60000));
    QCOMPARE(metadata.frameRateNumerator, quint32(30));
    QCOMPARE(metadata.frameRateDenominator, quint32(1));

    const QList<QPair<QString, quint32>> canonicalNonReadyStatuses = {
        {QStringLiteral("Partial"), quint32(0x1)},
        {QStringLiteral("Unavailable"), quint32(0)},
        {QStringLiteral("ProbeFailed"), quint32(0)},
    };
    int expectedReadyCount = ready.count();
    for (const auto &[status, availableFields] :
         canonicalNonReadyStatuses) {
        QVERIFY(service.invoke(
            [&runtime, &status, availableFields]() {
                runtime.deviceMediaMetadata.status = status;
                runtime.deviceMediaMetadata.availableFields =
                    availableFields;
                runtime.deviceMediaMetadata.durationMilliseconds = 0;
                runtime.deviceMediaMetadata.frameRateNumerator = 0;
                runtime.deviceMediaMetadata.frameRateDenominator = 0;
                if (availableFields == 0) {
                    runtime.deviceMediaMetadata.width = 0;
                    runtime.deviceMediaMetadata.height = 0;
                }
            }));
        QVERIFY(client.requestDeviceMediaMetadata(runtime.artifact));
        ++expectedReadyCount;
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), expectedReadyCount,
                                  3000);
        const TryxRuntimeDeviceMediaMetadataV1 next =
            qvariant_cast<TryxRuntimeDeviceMediaMetadataV1>(
                ready.constLast().constFirst());
        QCOMPARE(next.status, status);
        QCOMPARE(next.availableFields, availableFields);
    }
    QCOMPARE(failed.count(), 0);
}

void RuntimeClientHandshakeTests::
    malformedDeviceMediaMetadataFailsClosed_data() {
    QTest::addColumn<int>("malformation");

    QTest::newRow("schema") << 0;
    QTest::newRow("operation") << 1;
    QTest::newRow("artifact") << 2;
    QTest::newRow("media") << 3;
    QTest::newRow("device") << 4;
    QTest::newRow("hash") << 5;
    QTest::newRow("zero-generation") << 6;
    QTest::newRow("unknown-status") << 7;
    QTest::newRow("unknown-mask-bit") << 8;
    QTest::newRow("ready-with-partial-mask") << 9;
    QTest::newRow("zero-width") << 10;
    QTest::newRow("absent-duration-with-value") << 11;
    QTest::newRow("present-duration-with-zero") << 12;
    QTest::newRow("present-frame-rate-with-zero-numerator") << 13;
    QTest::newRow("present-frame-rate-with-zero-denominator") << 14;
    QTest::newRow("unavailable-with-nonempty-values") << 15;
}

void RuntimeClientHandshakeTests::
    malformedDeviceMediaMetadataFailsClosed() {
    QFETCH(int, malformation);

    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    TryxRuntimeDeviceMediaMetadataV1 &metadata =
        runtime.deviceMediaMetadata;
    switch (malformation) {
    case 0:
        metadata.schemaVersion = 2;
        break;
    case 1:
        metadata.operationId = QStringLiteral("operation-b");
        break;
    case 2:
        metadata.artifactId = QStringLiteral("artifact-b");
        break;
    case 3:
        metadata.mediaId = QStringLiteral("media-b");
        break;
    case 4:
        metadata.deviceIdentity = QStringLiteral("device-b");
        break;
    case 5:
        metadata.decodedSha256 = QString(64, QLatin1Char('b'));
        break;
    case 6:
        metadata.deviceGeneration = 0;
        break;
    case 7:
        metadata.status = QStringLiteral("Maybe");
        break;
    case 8:
        metadata.availableFields = 0xfU;
        break;
    case 9:
        metadata.availableFields = 0x1U;
        metadata.durationMilliseconds = 0;
        metadata.frameRateNumerator = 0;
        metadata.frameRateDenominator = 0;
        break;
    case 10:
        metadata.width = 0;
        break;
    case 11:
        metadata.status = QStringLiteral("Partial");
        metadata.availableFields = 0x5U;
        break;
    case 12:
        metadata.durationMilliseconds = 0;
        break;
    case 13:
        metadata.frameRateNumerator = 0;
        break;
    case 14:
        metadata.frameRateDenominator = 0;
        break;
    case 15:
        metadata.status = QStringLiteral("Unavailable");
        metadata.availableFields = 0;
        break;
    default:
        QFAIL("Unknown metadata malformation fixture");
    }

    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());
    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QSignalSpy ready(
        &client, &RuntimeClient::deviceMediaMetadataReady);
    QSignalSpy failed(
        &client, &RuntimeClient::deviceMediaMetadataFailed);
    QVERIFY(ready.isValid());
    QVERIFY(failed.isValid());

    QVERIFY(client.requestDeviceMediaMetadata(runtime.artifact));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(
        runtime.deviceMediaMetadataCalls.load(
            std::memory_order_acquire),
        1);
}

void RuntimeClientHandshakeTests::
    deviceMediaMetadataFailureDoesNotInvalidateArtifactClaim() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    runtime.deviceMediaMetadataReplyMode =
        CapabilityRuntimeObject::DeviceMediaMetadataReplyMode::Error;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QSignalSpy artifactReady(&client, &RuntimeClient::artifactClaimed);
    QSignalSpy artifactFailed(
        &client, &RuntimeClient::artifactClaimFailed);
    QSignalSpy metadataReady(
        &client, &RuntimeClient::deviceMediaMetadataReady);
    QSignalSpy metadataFailed(
        &client, &RuntimeClient::deviceMediaMetadataFailed);
    QVERIFY(artifactReady.isValid());
    QVERIFY(artifactFailed.isValid());
    QVERIFY(metadataReady.isValid());
    QVERIFY(metadataFailed.isValid());

    client.claimDeviceMediaArtifact(
        runtime.artifact.operationId,
        runtime.artifact.artifactId);
    QTRY_COMPARE_WITH_TIMEOUT(artifactReady.count(), 1, 3000);
    QCOMPARE(artifactFailed.count(), 0);
    const TryxRuntimeDeviceMediaArtifact artifact =
        qvariant_cast<TryxRuntimeDeviceMediaArtifact>(
            artifactReady.constFirst().at(1));

    QVERIFY(client.requestDeviceMediaMetadata(artifact));
    QTRY_COMPARE_WITH_TIMEOUT(metadataFailed.count(), 1, 3000);
    QCOMPARE(metadataReady.count(), 0);
    QCOMPARE(artifactReady.count(), 1);
    QCOMPARE(artifactFailed.count(), 0);
    QCOMPARE(artifact.artifactId, runtime.artifact.artifactId);
    QCOMPARE(artifact.leaseId, runtime.artifact.leaseId);

    QVERIFY(service.invoke([&runtime]() {
        runtime.deviceMediaMetadataReplyMode =
            CapabilityRuntimeObject::
                DeviceMediaMetadataReplyMode::UnknownMethod;
    }));
    QVERIFY(client.requestDeviceMediaMetadata(artifact));
    QTRY_COMPARE_WITH_TIMEOUT(metadataFailed.count(), 2, 3000);
    QCOMPARE(metadataReady.count(), 0);
    QCOMPARE(artifactReady.count(), 1);
    QCOMPARE(artifactFailed.count(), 0);
    QCOMPARE(
        runtime.deviceMediaMetadataCalls.load(
            std::memory_order_acquire),
        2);
}

void RuntimeClientHandshakeTests::
    newArtifactSupersedesPendingDeviceMediaMetadataRequest() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    runtime.deviceMediaMetadataReplyMode =
        CapabilityRuntimeObject::DeviceMediaMetadataReplyMode::Delayed;
    const TryxRuntimeDeviceMediaMetadataV1 staleMetadata =
        runtime.deviceMediaMetadata;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QSignalSpy ready(
        &client, &RuntimeClient::deviceMediaMetadataReady);
    QSignalSpy failed(
        &client, &RuntimeClient::deviceMediaMetadataFailed);
    QVERIFY(ready.isValid());
    QVERIFY(failed.isValid());
    QVERIFY(client.requestDeviceMediaMetadata(runtime.artifact));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceMediaMetadataCalls.load(
            std::memory_order_acquire),
        1, 3000);

    TryxRuntimeDeviceMediaArtifact currentArtifact = runtime.artifact;
    currentArtifact.operationId = QStringLiteral(
        "33333333-3333-4333-8333-333333333333");
    currentArtifact.artifactId = QStringLiteral(
        "44444444-4444-4444-8444-444444444444");
    currentArtifact.mediaId = QString(64, QLatin1Char('c'));
    currentArtifact.decodedSha256 = QString(64, QLatin1Char('d'));
    currentArtifact.leaseId = QStringLiteral("lease-b");
    TryxRuntimeDeviceMediaMetadataV1 currentMetadata =
        readyDeviceMediaMetadata();
    currentMetadata.operationId = currentArtifact.operationId;
    currentMetadata.artifactId = currentArtifact.artifactId;
    currentMetadata.mediaId = currentArtifact.mediaId;
    currentMetadata.deviceIdentity = currentArtifact.deviceIdentity;
    currentMetadata.decodedSha256 = currentArtifact.decodedSha256;
    currentMetadata.durationMilliseconds = 90000;
    QVERIFY(service.invoke(
        [&runtime, &currentArtifact, &currentMetadata]() {
            runtime.artifact = currentArtifact;
            runtime.deviceMediaMetadata = currentMetadata;
            runtime.deviceMediaMetadataReplyMode =
                CapabilityRuntimeObject::
                    DeviceMediaMetadataReplyMode::Normal;
        }));

    QVERIFY(client.requestDeviceMediaMetadata(currentArtifact));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    const TryxRuntimeDeviceMediaMetadataV1 received =
        qvariant_cast<TryxRuntimeDeviceMediaMetadataV1>(
            ready.constFirst().constFirst());
    QCOMPARE(received.artifactId, currentArtifact.artifactId);
    QCOMPARE(received.durationMilliseconds, quint64(90000));
    QCOMPARE(failed.count(), 0);

    bool staleReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &staleMetadata,
         &staleReplySent]() {
            staleReplySent =
                runtime.sendDelayedDeviceMediaMetadataReply(
                    service.connection(), staleMetadata);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void RuntimeClientHandshakeTests::
    staleDeviceMediaMetadataReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    firstRuntime.deviceMediaMetadataReplyMode =
        CapabilityRuntimeObject::DeviceMediaMetadataReplyMode::Delayed;
    const TryxRuntimeDeviceMediaMetadataV1 staleMetadata =
        firstRuntime.deviceMediaMetadata;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QSignalSpy ready(
        &client, &RuntimeClient::deviceMediaMetadataReady);
    QSignalSpy failed(
        &client, &RuntimeClient::deviceMediaMetadataFailed);
    QVERIFY(ready.isValid());
    QVERIFY(failed.isValid());
    QVERIFY(client.requestDeviceMediaMetadata(firstRuntime.artifact));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.deviceMediaMetadataCalls.load(
            std::memory_order_acquire),
        1, 3000);

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeCapabilities.append(
        tryxRuntimeDeviceMediaMetadataV1Token());
    secondRuntime.deviceMediaMetadata.durationMilliseconds = 90000;
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token()),
        3000);
    QVERIFY(client.requestDeviceMediaMetadata(secondRuntime.artifact));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    const TryxRuntimeDeviceMediaMetadataV1 currentMetadata =
        qvariant_cast<TryxRuntimeDeviceMediaMetadataV1>(
            ready.constFirst().constFirst());
    QCOMPARE(currentMetadata.durationMilliseconds, quint64(90000));

    const int readyCountBeforeStale = ready.count();
    const int failedCountBeforeStale = failed.count();
    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleMetadata,
         &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedDeviceMediaMetadataReply(
                    firstService.connection(), staleMetadata);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(ready.count(), readyCountBeforeStale);
    QCOMPARE(failed.count(), failedCountBeforeStale);
}

void RuntimeClientHandshakeTests::initTestCase() {
    QVERIFY2(
        QDBusConnection::sessionBus().isConnected(),
        "runtimeclient-handshake-tests requires dbus-run-session");
    registerTryxRuntimeMetaTypes();
}

void RuntimeClientHandshakeTests::
    fullMetricsCatalogUsesExactUniqueOwner() {
    CapabilityRuntimeObject runtime;
    runtime.metricsCatalog = tryxMetricsCatalog();
    runtime.metrics.revision = 1;
    runtime.metrics.deviceSerial = QStringLiteral("device-a");
    runtime.metrics.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QCOMPARE(client.metricsCatalog(), tryxMetricsCatalog());
    QCOMPARE(
        client.availableMetrics(),
        QStringList{QStringLiteral("CPU Temperature")});

    QVERIFY(client.runtimeOwner_.startsWith(QLatin1Char(':')));
    QCOMPARE(
        client.runtimeOwner_, service.connection().baseService());
    QVERIFY(client.runtimeOwner_ != tryxRuntimeServiceName());
}

void RuntimeClientHandshakeTests::
    metricsCatalogIsSeparateFromLiveAvailability() {
    CapabilityRuntimeObject runtime;
    runtime.metrics.revision = 1;
    runtime.metrics.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QCOMPARE(client.metricsCatalog(), runtime.metricsCatalog);
    QCOMPARE(
        client.availableMetrics(),
        QStringList{QStringLiteral("CPU Temperature")});
    QVERIFY(client.metricsCatalog().contains(
        QStringLiteral("GPU Temperature")));
    QVERIFY(!client.availableMetrics().contains(
        QStringLiteral("GPU Temperature")));
}

void RuntimeClientHandshakeTests::
    missingMetricsCatalogMethodUsesBoundedFallback() {
    LegacyRuntimeObject runtime;
    runtime.metrics.revision = 1;
    runtime.metrics.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    runtime.display.revision = 1;
    runtime.display.valid = true;
    runtime.display.deviceSerial = QStringLiteral("device-a");
    runtime.display.screenMode = QStringLiteral("Full Screen");
    runtime.display.sysinfoLabels = {
        QStringLiteral("GPU Temperature")};
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        client.metricsCatalog(),
        QStringList({
            QStringLiteral("CPU Temperature"),
            QStringLiteral("GPU Temperature"),
        }),
        3000);
}

void RuntimeClientHandshakeTests::
    onlyExactUnknownMethodUsesBoundedFallback() {
    CapabilityRuntimeObject runtime;
    runtime.metricsCatalogReplyMode =
        CapabilityRuntimeObject::MetricsCatalogReplyMode::Error;
    runtime.metricsCatalogError = QDBusError::UnknownMethod;
    runtime.metricsCatalogErrorMessage =
        QStringLiteral("legacy catalog method unavailable");
    runtime.metrics.revision = 1;
    runtime.metrics.deviceSerial = QStringLiteral("device-a");
    runtime.metrics.availableMetrics = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("Unknown Metric"),
        QStringLiteral("CPU Temperature")};
    runtime.metrics.metrics = {
        QStringLiteral("GPU Power"),
        QStringLiteral("CPU Temperature")};
    runtime.display.revision = 1;
    runtime.display.valid = true;
    runtime.display.deviceSerial = QStringLiteral("device-a");
    runtime.display.screenMode = QStringLiteral("Screen Splitting");
    runtime.display.sysinfoLabels = {
        QStringLiteral("Memory Usage"),
        QStringLiteral("GPU Power")};
    runtime.display.sysinfoLabels2 = {
        QStringLiteral("Date&Time"),
        QStringLiteral("Unknown Metric")};
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        client.metricsCatalog(),
        QStringList({QStringLiteral("CPU Temperature"),
                     QStringLiteral("GPU Power"),
                     QStringLiteral("Memory Usage"),
                     QStringLiteral("Date&Time")}),
        3000);
}

void RuntimeClientHandshakeTests::
    malformedMetricsCatalogFailsClosed_data() {
    QTest::addColumn<QStringList>("catalog");

    QTest::newRow("empty") << QStringList{};
    QTest::newRow("duplicate")
        << QStringList({
               QStringLiteral("CPU Temperature"),
               QStringLiteral("CPU Temperature"),
           });
    QTest::newRow("unknown")
        << QStringList{QStringLiteral("Disk Temperature")};
    QTest::newRow("wrong-case")
        << QStringList{QStringLiteral("cpu temperature")};
    QTest::newRow("leading-space")
        << QStringList{QStringLiteral(" CPU Temperature")};
    QTest::newRow("trailing-space")
        << QStringList{QStringLiteral("CPU Temperature ")};
    QTest::newRow("oversized-token")
        << QStringList{QString(65, QLatin1Char('x'))};
    QStringList oversizedCatalog;
    for (int index = 0; index < 12; ++index) {
        oversizedCatalog.append(QStringLiteral("CPU Temperature"));
    }
    QTest::newRow("oversized-catalog") << oversizedCatalog;
}

void RuntimeClientHandshakeTests::
    malformedMetricsCatalogFailsClosed() {
    QFETCH(QStringList, catalog);
    CapabilityRuntimeObject runtime;
    runtime.metricsCatalog = catalog;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QStringList observedDiagnostics;
    connect(
        &client, &RuntimeClient::diagnosticChanged, &client,
        [&client, &observedDiagnostics]() {
            observedDiagnostics.append(client.diagnostic());
        });
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.metricsCatalogReady());
    QVERIFY(client.metricsCatalog().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        observedDiagnostics.contains(
        QStringLiteral(
            "The runtime returned an invalid metrics catalog")),
        3000);
}

void RuntimeClientHandshakeTests::
    nonUnknownMetricsCatalogErrorFailsClosed_data() {
    QTest::addColumn<int>("errorType");
    QTest::addColumn<QString>("errorMessage");

    QTest::newRow("access-denied")
        << int(QDBusError::AccessDenied)
        << QStringLiteral("metrics catalog denied");
    QTest::newRow("unknown-interface")
        << int(QDBusError::UnknownInterface)
        << QStringLiteral("metrics catalog interface unavailable");
    QTest::newRow("invalid-signature")
        << int(QDBusError::InvalidSignature)
        << QStringLiteral("metrics catalog signature invalid");
    QTest::newRow("failed")
        << int(QDBusError::Failed)
        << QStringLiteral("metrics catalog failed");
}

void RuntimeClientHandshakeTests::
    nonUnknownMetricsCatalogErrorFailsClosed() {
    QFETCH(int, errorType);
    QFETCH(QString, errorMessage);
    CapabilityRuntimeObject runtime;
    runtime.metricsCatalogReplyMode =
        CapabilityRuntimeObject::MetricsCatalogReplyMode::Error;
    runtime.metricsCatalogError =
        static_cast<QDBusError::ErrorType>(errorType);
    runtime.metricsCatalogErrorMessage = errorMessage;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QStringList observedDiagnostics;
    connect(
        &client, &RuntimeClient::diagnosticChanged, &client,
        [&client, &observedDiagnostics]() {
            observedDiagnostics.append(client.diagnostic());
        });
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.metricsCatalogReady());
    QVERIFY(client.metricsCatalog().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        observedDiagnostics.contains(errorMessage),
        3000);
}

void RuntimeClientHandshakeTests::
    staleMetricsCatalogReplyFromSupersededAttemptSameOwnerIsDiscarded() {
    CapabilityRuntimeObject runtime;
    runtime.metricsCatalogReplyMode =
        CapabilityRuntimeObject::MetricsCatalogReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.metricsCatalogReady());

    const QStringList currentCatalog = {
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time")};
    QVERIFY(service.invoke([&runtime, &currentCatalog]() {
        runtime.metricsCatalogReplyMode =
            CapabilityRuntimeObject::MetricsCatalogReplyMode::Normal;
        runtime.metricsCatalog = currentCatalog;
    }));
    const QString serviceName = tryxRuntimeServiceName();
    QVERIFY(QMetaObject::invokeMethod(
        &client, "onServiceRegistered", Qt::DirectConnection,
        Q_ARG(QString, serviceName)));

    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QCOMPARE(client.metricsCatalog(), currentCatalog);

    bool staleReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &staleReplySent]() {
            staleReplySent =
                runtime.sendDelayedMetricsCatalogReply(
                    service.connection(),
                    {QStringLiteral("GPU Power")});
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.metricsCatalog(), currentCatalog);
}

void RuntimeClientHandshakeTests::
    staleMetricsCatalogReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.metricsCatalogReplyMode =
        CapabilityRuntimeObject::MetricsCatalogReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.metricsCatalogCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.metricsCatalogReady());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.metricsCatalog = {
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time"),
    };
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QCOMPARE(client.metricsCatalog(), secondRuntime.metricsCatalog);

    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedMetricsCatalogReply(
                    firstService.connection(),
                    {QStringLiteral("GPU Power")});
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.metricsCatalog(), secondRuntime.metricsCatalog);
}

void RuntimeClientHandshakeTests::
    currentOwnerMetricsAndDisplaySignalsAreAccepted() {
    CapabilityRuntimeObject runtime;
    runtime.metrics.revision = 1;
    runtime.metrics.deviceSerial = QStringLiteral("device-a");
    runtime.metrics.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    runtime.display.revision = 1;
    runtime.display.deviceSerial = QStringLiteral("device-a");
    runtime.display.valid = true;
    runtime.display.brightness = 41;
    runtime.display.screenMode = QStringLiteral("Full Screen");
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        client.availableMetrics(),
        QStringList{QStringLiteral("CPU Temperature")}, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.brightness(), 41, 3000);

    TryxRuntimeMetricsState metrics = runtime.metrics;
    metrics.revision = 2;
    metrics.availableMetrics = {
        QStringLiteral("GPU Power")};
    TryxRuntimeDisplayState display = runtime.display;
    display.revision = 2;
    display.brightness = 77;
    QVERIFY(service.sendMetricsStateUpdated(metrics));
    QVERIFY(service.sendDisplayStateUpdated(display));

    QTRY_COMPARE_WITH_TIMEOUT(
        client.availableMetrics(),
        QStringList{QStringLiteral("GPU Power")}, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.brightness(), 77, 3000);
}

void RuntimeClientHandshakeTests::
    legacyApi8KeepsBaselineAndUsesEmptyCapabilities() {
    LegacyRuntimeObject runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.compatible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.connected(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QCOMPARE(client.apiVersion(), tryxRuntimeApiVersion());
    QCOMPARE(client.runtimeCapabilities(), QStringList());
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());
    QCOMPARE(client.deviceSpecificationsStatus(),
             QStringLiteral("NotSupported"));
    QVERIFY(!client.deviceSpecificationsSupported());
    QVERIFY(!client.deviceSpecificationsReady());
    QVERIFY(!client.presentationPreferencesReady());
    QVERIFY(!client.presentationPreferencesBusy());
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("24H"));
    QCOMPARE(client.formatTemperature(true, -2.5),
             QStringLiteral("-3 \u00b0C"));
    QCOMPARE(client.formatTemperature(false, 50.0),
             QStringLiteral("\u2014"));
}

void RuntimeClientHandshakeTests::
    nonLegacyCapabilityErrorFailsClosed_data() {
    QTest::addColumn<int>("replyMode");
    QTest::addColumn<QString>("expectedDiagnostic");

    QTest::newRow("access-denied")
        << int(CapabilityRuntimeObject::
                   RuntimeReplyMode::AccessDenied)
        << QStringLiteral("capability access denied");
    QTest::newRow("unknown-interface")
        << int(CapabilityRuntimeObject::
                   RuntimeReplyMode::UnknownInterface)
        << QStringLiteral("capability interface unavailable");
    QTest::newRow("invalid-signature")
        << int(CapabilityRuntimeObject::
                   RuntimeReplyMode::InvalidSignature)
        << QStringLiteral("invalid capability signature");
}

void RuntimeClientHandshakeTests::nonLegacyCapabilityErrorFailsClosed() {
    QFETCH(int, replyMode);
    QFETCH(QString, expectedDiagnostic);

    CapabilityRuntimeObject runtime;
    runtime.runtimeReplyMode =
        static_cast<CapabilityRuntimeObject::RuntimeReplyMode>(
            replyMode);
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QStringList observedDiagnostics;
    connect(
        &client, &RuntimeClient::diagnosticChanged, &client,
        [&client, &observedDiagnostics]() {
            observedDiagnostics.append(client.diagnostic());
        });
    QTRY_VERIFY_WITH_TIMEOUT(client.compatible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.runtimeCapabilityCalls.load(
            std::memory_order_acquire) > 0,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        observedDiagnostics.contains(expectedDiagnostic), 3000);
    QVERIFY(!client.capabilitiesReady());
    QCOMPARE(client.runtimeCapabilities(), QStringList());
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());
    QCOMPARE(client.deviceSpecificationsStatus(),
             QStringLiteral("RuntimeUnavailable"));
}

void RuntimeClientHandshakeTests::
    validCapabilitiesAreFilteredAndDeviceBound() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities = {
        tryxRuntimeDeviceCapabilitiesV1Token(),
        tryxRuntimeMediaPreparationProfileV1Token(),
        QStringLiteral("runtime.unknown.v1"),
        tryxRuntimeDeviceCapabilitiesV1Token(),
        QStringLiteral(" runtime.device-capabilities.v1"),
    };
    runtime.deviceCapabilities.capabilities = {
        tryxDeviceMediaUploadV1Token(),
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
        QStringLiteral("device.unknown.v1"),
        tryxDeviceMediaCatalogV1Token(),
        tryxDeviceMediaUploadV1Token(),
    };
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.runtimeCapabilities(),
        QStringList({
            tryxRuntimeDeviceCapabilitiesV1Token(),
            tryxRuntimeMediaPreparationProfileV1Token(),
        }));
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList({
            tryxDeviceMediaUploadV1Token(),
            tryxDeviceDisplayConfigurationV1Token(),
            tryxDeviceMediaSplitAreaV1Token(),
            tryxDeviceMediaCatalogV1Token(),
        }));
    QVERIFY(client.hasRuntimeCapability(
        tryxRuntimeDeviceCapabilitiesV1Token()));
    QVERIFY(client.hasDeviceCapability(
        tryxDeviceMediaUploadV1Token()));
    QVERIFY(client.hasRuntimeCapability(
        tryxRuntimeMediaPreparationProfileV1Token()));
    QVERIFY(client.hasDeviceCapability(
        tryxDeviceMediaSplitAreaV1Token()));
    QVERIFY(!client.hasDeviceCapability(
        QStringLiteral("device.unknown.v1")));
}

void RuntimeClientHandshakeTests::
    deviceSpecificationsAreCapabilityGatedAndDeviceBound() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceSpecificationsStatus(), QStringLiteral("Ready"), 3000);
    QVERIFY(client.deviceSpecificationsSupported());
    QVERIFY(client.deviceSpecificationsReady());
    QCOMPARE(client.deviceReportedProductName(), QStringLiteral("PASE"));
    QCOMPARE(client.deviceVideoOutputWidth(), 2240);
    QCOMPARE(client.deviceVideoOutputHeight(), 1080);
    QCOMPARE(client.deviceScreenType(), QStringLiteral("OLED"));
    QCOMPARE(client.deviceUsbAutoKeepalive(), false);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);
    processEventsFor(200);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);
}

void RuntimeClientHandshakeTests::
    deviceSpecificationsContextStatesAreExact() {
    {
        CapabilityRuntimeObject runtime;
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());
        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
        QCOMPARE(client.deviceSpecificationsStatus(),
                 QStringLiteral("NotSupported"));
        QVERIFY(!client.deviceSpecificationsSupported());
        QCOMPARE(runtime.deviceSpecificationsCalls.load(), 0);
    }
    {
        CapabilityRuntimeObject runtime;
        runtime.runtimeCapabilities.append(
            tryxRuntimeDeviceSpecificationsV1Token());
        runtime.connection = {};
        runtime.connection.revision = 1;
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());
        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            client.deviceSpecificationsStatus(),
            QStringLiteral("Disconnected"), 3000);
        QCOMPARE(runtime.deviceCapabilityCalls.load(), 0);
        QCOMPARE(runtime.deviceSpecificationsCalls.load(), 0);
    }
    {
        CapabilityRuntimeObject runtime;
        runtime.runtimeCapabilities.append(
            tryxRuntimeDeviceSpecificationsV1Token());
        runtime.connection.printerClassConnected = false;
        runtime.connection.printerClassDevicePresent = false;
        runtime.connection.productId = QStringLiteral("legacy");
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());
        RuntimeClient client;
        QTRY_COMPARE_WITH_TIMEOUT(
            client.deviceSpecificationsStatus(),
            QStringLiteral("Unsupported"), 3000);
        QCOMPARE(runtime.deviceCapabilityCalls.load(), 0);
        QCOMPARE(runtime.deviceSpecificationsCalls.load(), 0);
    }
    {
        CapabilityRuntimeObject runtime;
        runtime.runtimeCapabilities = {
            tryxRuntimeDeviceSpecificationsV1Token()};
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());
        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.connected(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            client.deviceSpecificationsStatus(),
            QStringLiteral("RuntimeUnavailable"), 3000);
        QCOMPARE(runtime.deviceCapabilityCalls.load(), 0);
        QCOMPARE(runtime.deviceSpecificationsCalls.load(), 0);
    }
    {
        CapabilityRuntimeObject runtime;
        runtime.runtimeCapabilities.append(
            tryxRuntimeDeviceSpecificationsV1Token());
        runtime.connection.productId = QStringLiteral("2011");
        runtime.deviceSpecifications.status = QStringLiteral("Unsupported");
        runtime.deviceSpecifications.reportedProductName.clear();
        runtime.deviceSpecifications.videoOutputWidth = 0;
        runtime.deviceSpecifications.videoOutputHeight = 0;
        runtime.deviceSpecifications.screenType.clear();
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());
        RuntimeClient client;
        QTRY_COMPARE_WITH_TIMEOUT(
            client.deviceSpecificationsStatus(),
            QStringLiteral("Unsupported"), 3000);
        QCOMPARE(runtime.deviceCapabilityCalls.load(), 1);
        QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);
        QVERIFY(!client.deviceSpecificationsReady());
    }
}

void RuntimeClientHandshakeTests::
    deviceSpecificationsGetterErrorsFailClosed_data() {
    QTest::addColumn<int>("replyMode");
    QTest::newRow("access-denied")
        << int(CapabilityRuntimeObject::
                   DeviceSpecificationsReplyMode::Error);
    QTest::newRow("advertised-unknown-method")
        << int(CapabilityRuntimeObject::
                   DeviceSpecificationsReplyMode::UnknownMethod);
}

void RuntimeClientHandshakeTests::
    deviceSpecificationsGetterErrorsFailClosed() {
    QFETCH(int, replyMode);
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    runtime.deviceSpecificationsReplyMode =
        static_cast<CapabilityRuntimeObject::
            DeviceSpecificationsReplyMode>(replyMode);
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceSpecificationsStatus(),
        QStringLiteral("RuntimeUnavailable"), 3000);
    QVERIFY(client.deviceSpecificationsSupported());
    QVERIFY(!client.deviceSpecificationsReady());
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);
}

void RuntimeClientHandshakeTests::
    malformedDeviceSpecificationsFailClosed_data() {
    QTest::addColumn<int>("malformation");
    QTest::newRow("schema") << 0;
    QTest::newRow("status") << 1;
    QTest::newRow("identity") << 2;
    QTest::newRow("revision") << 3;
    QTest::newRow("generation") << 4;
    QTest::newRow("non-ready-payload") << 5;
    QTest::newRow("bidi-product") << 6;
    QTest::newRow("zero-width") << 7;
    QTest::newRow("oversized-width") << 8;
    QTest::newRow("screen-type") << 9;
    QTest::newRow("untrimmed-product") << 10;
}

void RuntimeClientHandshakeTests::
    malformedDeviceSpecificationsFailClosed() {
    QFETCH(int, malformation);
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    switch (malformation) {
    case 0:
        runtime.deviceSpecifications.schemaVersion = 2;
        break;
    case 1:
        runtime.deviceSpecifications.status = QStringLiteral("ready");
        break;
    case 2:
        runtime.deviceSpecifications.deviceIdentity =
            QStringLiteral("device-b");
        break;
    case 3:
        runtime.deviceSpecifications.connectionRevision = 2;
        break;
    case 4:
        runtime.deviceSpecifications.physicalGeneration = 8;
        break;
    case 5:
        runtime.deviceSpecifications.status = QStringLiteral("Unavailable");
        break;
    case 6:
        runtime.deviceSpecifications.reportedProductName =
            QStringLiteral("PA\u202eSE");
        break;
    case 7:
        runtime.deviceSpecifications.videoOutputWidth = 0;
        break;
    case 8:
        runtime.deviceSpecifications.videoOutputWidth = 16385;
        break;
    case 9:
        runtime.deviceSpecifications.screenType = QStringLiteral("MiniLED");
        break;
    case 10:
        runtime.deviceSpecifications.reportedProductName =
            QStringLiteral(" PASE ");
        break;
    default:
        QFAIL("unknown device specifications malformation");
    }
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceSpecificationsStatus(),
        QStringLiteral("RuntimeUnavailable"), 3000);
    QVERIFY(!client.deviceSpecificationsReady());
    QVERIFY(client.deviceReportedProductName().isEmpty());
    QCOMPARE(client.deviceVideoOutputWidth(), 0);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);
}

void RuntimeClientHandshakeTests::
    displaySessionRefreshTransitionsSpecificationsToReady() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    runtime.deviceSpecifications.status = QStringLiteral("Unavailable");
    runtime.deviceSpecifications.reportedProductName.clear();
    runtime.deviceSpecifications.videoOutputWidth = 0;
    runtime.deviceSpecifications.videoOutputHeight = 0;
    runtime.deviceSpecifications.screenType.clear();
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceSpecificationsStatus(),
        QStringLiteral("Unavailable"), 3000);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 1);

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(2);
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceSpecifications.connectionRevision = 2;
        runtime.deviceSpecifications.status = QStringLiteral("Ready");
        runtime.deviceSpecifications.reportedProductName =
            QStringLiteral("PASE READY");
        runtime.deviceSpecifications.videoOutputWidth = 2240;
        runtime.deviceSpecifications.videoOutputHeight = 1080;
        runtime.deviceSpecifications.screenType = QStringLiteral("OLED");
    }));
    QVERIFY(service.sendDisplaySessionChanged(true, 2));
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceReportedProductName(),
        QStringLiteral("PASE READY"), 3000);
    QCOMPARE(client.deviceSpecificationsStatus(), QStringLiteral("Ready"));
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 2);
    processEventsFor(200);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 2);
}

void RuntimeClientHandshakeTests::
    staleDeviceSpecificationsReplyAfterRevisionChangeIsDiscarded() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    runtime.deviceSpecificationsReplyMode = CapabilityRuntimeObject::
        DeviceSpecificationsReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceSpecificationsCalls.load(
            std::memory_order_acquire),
        1, 3000);
    const TryxRuntimeDeviceSpecificationsV1 staleSpecifications =
        runtime.deviceSpecifications;

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(2);
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceCapabilities.physicalGeneration = 8;
        runtime.deviceSpecifications.connectionRevision = 2;
        runtime.deviceSpecifications.physicalGeneration = 8;
        runtime.deviceSpecifications.reportedProductName =
            QStringLiteral("PASE NEXT");
        runtime.deviceSpecificationsReplyMode = CapabilityRuntimeObject::
            DeviceSpecificationsReplyMode::Normal;
    }));
    client.refreshAll();
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceReportedProductName(),
        QStringLiteral("PASE NEXT"), 3000);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 2);

    bool staleReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &staleSpecifications, &staleReplySent]() {
            staleReplySent =
                runtime.sendDelayedDeviceSpecificationsReply(
                    service.connection(), staleSpecifications);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.deviceReportedProductName(),
             QStringLiteral("PASE NEXT"));
    QCOMPARE(client.deviceSpecificationsStatus(), QStringLiteral("Ready"));
}

void RuntimeClientHandshakeTests::
    printerOperationsCancellationFencesDeviceSpecifications() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    runtime.deviceSpecificationsReplyMode = CapabilityRuntimeObject::
        DeviceSpecificationsReplyMode::Delayed;
    const TryxRuntimeDeviceSpecificationsV1 staleSpecifications =
        runtime.deviceSpecifications;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceSpecificationsCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QCOMPARE(client.deviceSpecificationsStatus(),
             QStringLiteral("Unavailable"));

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(2);
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceCapabilities.physicalGeneration = 8;
        runtime.deviceSpecifications.connectionRevision = 2;
        runtime.deviceSpecifications.physicalGeneration = 8;
        runtime.deviceSpecifications.reportedProductName =
            QStringLiteral("PASE AFTER CANCELLATION");
        runtime.deviceSpecificationsReplyMode = CapabilityRuntimeObject::
            DeviceSpecificationsReplyMode::Normal;
    }));
    QVERIFY(service.sendPrinterOperationsCancelled(2));
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceReportedProductName(),
        QStringLiteral("PASE AFTER CANCELLATION"), 3000);
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 2);

    bool staleReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &staleSpecifications, &staleReplySent]() {
            staleReplySent =
                runtime.sendDelayedDeviceSpecificationsReply(
                    service.connection(), staleSpecifications);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.deviceReportedProductName(),
             QStringLiteral("PASE AFTER CANCELLATION"));
    QCOMPARE(client.deviceSpecificationsStatus(), QStringLiteral("Ready"));
}

void RuntimeClientHandshakeTests::
    staleDeviceSpecificationsReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    firstRuntime.deviceSpecificationsReplyMode = CapabilityRuntimeObject::
        DeviceSpecificationsReplyMode::Delayed;
    firstRuntime.deviceSpecifications.reportedProductName =
        QStringLiteral("PASE FIRST OWNER");
    const TryxRuntimeDeviceSpecificationsV1 staleSpecifications =
        firstRuntime.deviceSpecifications;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.deviceSpecificationsCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    secondRuntime.deviceSpecifications.reportedProductName =
        QStringLiteral("PASE SECOND OWNER");
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_COMPARE_WITH_TIMEOUT(
        client.deviceReportedProductName(),
        QStringLiteral("PASE SECOND OWNER"), 3000);

    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleSpecifications,
         &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedDeviceSpecificationsReply(
                    firstService.connection(), staleSpecifications);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.deviceReportedProductName(),
             QStringLiteral("PASE SECOND OWNER"));
    QCOMPARE(client.deviceSpecificationsStatus(), QStringLiteral("Ready"));
}

void RuntimeClientHandshakeTests::
    invalidDeviceCapabilitiesStayFailClosed_data() {
    QTest::addColumn<quint32>("schemaVersion");
    QTest::addColumn<QString>("identity");
    QTest::addColumn<quint64>("connectionRevision");
    QTest::addColumn<quint64>("physicalGeneration");

    QTest::newRow("schema")
        << quint32(2) << QStringLiteral("device-a")
        << quint64(1) << quint64(7);
    QTest::newRow("empty-identity")
        << quint32(1) << QString()
        << quint64(1) << quint64(7);
    QTest::newRow("identity")
        << quint32(1) << QStringLiteral("device-b")
        << quint64(1) << quint64(7);
    QTest::newRow("revision")
        << quint32(1) << QStringLiteral("device-a")
        << quint64(2) << quint64(7);
    QTest::newRow("generation")
        << quint32(1) << QStringLiteral("device-a")
        << quint64(1) << quint64(0);
}

void RuntimeClientHandshakeTests::
    invalidDeviceCapabilitiesStayFailClosed() {
    QFETCH(quint32, schemaVersion);
    QFETCH(QString, identity);
    QFETCH(quint64, connectionRevision);
    QFETCH(quint64, physicalGeneration);

    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeDeviceSpecificationsV1Token());
    runtime.deviceCapabilities.schemaVersion = schemaVersion;
    runtime.deviceCapabilities.deviceIdentity = identity;
    runtime.deviceCapabilities.connectionRevision =
        connectionRevision;
    runtime.deviceCapabilities.physicalGeneration =
        physicalGeneration;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    const QString invalidContextDiagnostic = QStringLiteral(
        "The runtime returned device capabilities for an "
        "invalid device context");
    QStringList observedDiagnostics;
    connect(
        &client, &RuntimeClient::diagnosticChanged, &client,
        [&client, &observedDiagnostics]() {
            observedDiagnostics.append(client.diagnostic());
        });
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire) > 0,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        observedDiagnostics.contains(invalidContextDiagnostic), 3000);
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());
    QCOMPARE(client.deviceSpecificationsStatus(),
             QStringLiteral("RuntimeUnavailable"));
    QCOMPARE(runtime.deviceSpecificationsCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    connectionRevisionInvalidatesAndRequeriesDeviceCapabilities() {
    CapabilityRuntimeObject runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(runtime.deviceCapabilityCalls.load(), 1);

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(
            2, QStringLiteral("device-a"));
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceCapabilities.physicalGeneration = 8;
        runtime.deviceCapabilities.capabilities = {
            tryxDeviceDisplayConfigurationV1Token(),
        };
        runtime.deviceReplyMode =
            CapabilityRuntimeObject::DeviceReplyMode::Delayed;
    }));

    client.refreshAll();
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire),
        2, 3000);
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());

    bool delayedDeviceReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &delayedDeviceReplySent]() {
            runtime.deviceReplyMode =
                CapabilityRuntimeObject::DeviceReplyMode::Normal;
            delayedDeviceReplySent =
                runtime.sendDelayedDeviceReply(
                    service.connection(),
                    runtime.deviceCapabilities);
        }));
    QVERIFY(delayedDeviceReplySent);
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});
}

void RuntimeClientHandshakeTests::
    generationOnlyCancellationInvalidatesAndRequeriesCapabilities() {
    CapabilityRuntimeObject runtime;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList({
            tryxDeviceMediaUploadV1Token(),
            tryxDeviceMediaCatalogV1Token(),
        }));

    const int initialConnectionSnapshotCalls =
        runtime.connectionSnapshotCalls.load(
            std::memory_order_acquire);
    const int initialDeviceCapabilityCalls =
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire);
    QSignalSpy capabilitiesChanged(
        &client, &RuntimeClient::capabilitiesChanged);

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(
            2, QStringLiteral("device-a"));
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceCapabilities.physicalGeneration = 8;
        runtime.deviceCapabilities.capabilities = {
            tryxDeviceDisplayConfigurationV1Token(),
        };
        runtime.deviceReplyMode =
            CapabilityRuntimeObject::DeviceReplyMode::Delayed;
    }));

    QVERIFY(service.sendPrinterOperationsCancelled(2));
    QTRY_VERIFY_WITH_TIMEOUT(capabilitiesChanged.count() > 0, 3000);
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.connectionSnapshotCalls.load(
            std::memory_order_acquire) >
            initialConnectionSnapshotCalls,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire) >
            initialDeviceCapabilityCalls,
        3000);
    QVERIFY(!client.deviceCapabilitiesReady());
    QCOMPARE(client.deviceCapabilities(), QStringList());

    bool delayedDeviceReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &delayedDeviceReplySent]() {
            runtime.deviceReplyMode =
                CapabilityRuntimeObject::DeviceReplyMode::Normal;
            delayedDeviceReplySent =
                runtime.sendDelayedDeviceReply(
                    service.connection(),
                    runtime.deviceCapabilities);
        }));
    QVERIFY(delayedDeviceReplySent);
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});
}

void RuntimeClientHandshakeTests::
    repeatedRegistrationDuringPendingApiHandshakeRestartsHandshake() {
    CapabilityRuntimeObject runtime;
    runtime.apiReplyMode =
        CapabilityRuntimeObject::ApiReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.apiCalls.load(std::memory_order_acquire),
        1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.serviceAvailable(), 3000);
    QVERIFY(!client.compatible());
    QVERIFY(!client.capabilitiesReady());
    QCOMPARE(
        runtime.connectionSnapshotCalls.load(
            std::memory_order_acquire),
        0);

    QVERIFY(service.invoke([&runtime]() {
        runtime.apiReplyMode =
            CapabilityRuntimeObject::ApiReplyMode::Normal;
        runtime.deviceCapabilities.physicalGeneration = 9;
        runtime.deviceCapabilities.capabilities = {
            tryxDeviceDisplayConfigurationV1Token(),
        };
    }));
    const QString serviceName = tryxRuntimeServiceName();
    QVERIFY(QMetaObject::invokeMethod(
        &client, "onServiceRegistered", Qt::DirectConnection,
        Q_ARG(QString, serviceName)));

    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.apiCalls.load(std::memory_order_acquire),
        2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.compatible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.runtimeCapabilities(),
        QStringList{tryxRuntimeDeviceCapabilitiesV1Token()});
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});

    bool delayedApiReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &delayedApiReplySent]() {
            delayedApiReplySent = runtime.sendDelayedApiReply(
                service.connection(), tryxRuntimeApiVersion());
        }));
    QVERIFY(delayedApiReplySent);
    processEventsFor(250);
    QVERIFY(client.compatible());
    QVERIFY(client.capabilitiesReady());
    QVERIFY(client.deviceCapabilitiesReady());
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});
}

void RuntimeClientHandshakeTests::
    staleDeviceReplyAfterRevisionChangeIsDiscarded() {
    CapabilityRuntimeObject runtime;
    runtime.deviceReplyMode =
        CapabilityRuntimeObject::DeviceReplyMode::Delayed;
    const TryxRuntimeDeviceCapabilitiesV1 staleCapabilities =
        runtime.deviceCapabilities;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.deviceCapabilitiesReady());

    QVERIFY(service.invoke([&runtime]() {
        runtime.connection = connectedSnapshot(
            2, QStringLiteral("device-a"));
        runtime.deviceCapabilities.connectionRevision = 2;
        runtime.deviceCapabilities.physicalGeneration = 8;
        runtime.deviceCapabilities.capabilities = {
            tryxDeviceDisplayConfigurationV1Token(),
        };
        runtime.deviceReplyMode =
            CapabilityRuntimeObject::DeviceReplyMode::Normal;
    }));
    client.refreshAll();
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.deviceCapabilityCalls.load(
            std::memory_order_acquire),
        2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.deviceCapabilitiesReady(), 3000);
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});

    bool delayedDeviceReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &staleCapabilities,
         &delayedDeviceReplySent]() {
            delayedDeviceReplySent =
                runtime.sendDelayedDeviceReply(
                    service.connection(), staleCapabilities);
        }));
    QVERIFY(delayedDeviceReplySent);
    processEventsFor(250);
    QVERIFY(client.deviceCapabilitiesReady());
    QCOMPARE(
        client.deviceCapabilities(),
        QStringList{tryxDeviceDisplayConfigurationV1Token()});
}

void RuntimeClientHandshakeTests::
    staleReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeReplyMode =
        CapabilityRuntimeObject::RuntimeReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.compatible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        firstRuntime.runtimeCapabilityCalls.load(
            std::memory_order_acquire) > 0,
        3000);
    QVERIFY(!client.capabilitiesReady());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeReplyMode =
        CapabilityRuntimeObject::RuntimeReplyMode::AccessDenied;
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.serviceAvailable(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.compatible(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        secondRuntime.runtimeCapabilityCalls.load(
            std::memory_order_acquire) > 0,
        3000);
    QVERIFY(!client.capabilitiesReady());

    bool delayedRuntimeReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService,
         &delayedRuntimeReplySent]() {
            delayedRuntimeReplySent =
                firstRuntime.sendDelayedRuntimeReply(
                    firstService.connection(),
                    QStringList{
                        tryxRuntimeDeviceCapabilitiesV1Token()});
        }));
    QVERIFY(delayedRuntimeReplySent);
    processEventsFor(250);
    QVERIFY(client.compatible());
    QVERIFY(!client.capabilitiesReady());
    QCOMPARE(client.runtimeCapabilities(), QStringList());
    QVERIFY(!client.deviceCapabilitiesReady());
}

void RuntimeClientHandshakeTests::
    presentationPreferencesAreConfirmedAndWritable() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("24H"));
    QCOMPARE(client.formatTemperature(true, 50.0),
             QStringLiteral("50 \u00b0C"));

    client.setPresentationPreferences(
        QStringLiteral("Fahrenheit"), QStringLiteral("12H"));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationSetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !client.presentationPreferencesBusy(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        client.temperatureUnit(), QStringLiteral("Fahrenheit"),
        3000);
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));
    QCOMPARE(client.formatTemperature(true, 50.0),
             QStringLiteral("122 \u00b0F"));

    TryxRuntimePresentationPreferencesV1 signalled;
    signalled.revision = 3;
    signalled.temperatureUnit = QStringLiteral("Celsius");
    signalled.timeFormat = QStringLiteral("12H");
    QVERIFY(service.sendPresentationPreferencesChanged(signalled));
    QTRY_COMPARE_WITH_TIMEOUT(
        client.temperatureUnit(), QStringLiteral("Celsius"),
        3000);
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));

    TryxRuntimePresentationPreferencesV1 equalRevision = signalled;
    equalRevision.temperatureUnit = QStringLiteral("Fahrenheit");
    QVERIFY(service.sendPresentationPreferencesChanged(equalRevision));
    QTRY_VERIFY_WITH_TIMEOUT(
        client.diagnostic().contains(
            QStringLiteral("reused a presentation preference revision"),
            Qt::CaseInsensitive),
        3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));
}

void RuntimeClientHandshakeTests::
    invalidPresentationPreferencesFailClosed() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    runtime.presentationPreferences.temperatureUnit =
        QStringLiteral("fahrenheit");
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationGetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        client.diagnostic().contains(
            QStringLiteral("invalid presentation preferences"),
            Qt::CaseInsensitive),
        3000);
    QVERIFY(!client.presentationPreferencesReady());
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("24H"));
}

void RuntimeClientHandshakeTests::
    presentationSignalBeforeGetterReplyWins() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    runtime.presentationGetterReplyMode =
        CapabilityRuntimeObject::PresentationReplyMode::Delayed;
    const TryxRuntimePresentationPreferencesV1 stalePreferences =
        runtime.presentationPreferences;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationGetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(!client.presentationPreferencesReady());

    TryxRuntimePresentationPreferencesV1 newer;
    newer.revision = 2;
    newer.temperatureUnit = QStringLiteral("Fahrenheit");
    newer.timeFormat = QStringLiteral("12H");
    QVERIFY(service.sendPresentationPreferencesChanged(newer));
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));

    bool delayedReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &stalePreferences,
         &delayedReplySent]() {
            delayedReplySent =
                runtime.sendDelayedPresentationGetterReply(
                    service.connection(), stalePreferences);
        }));
    QVERIFY(delayedReplySent);
    processEventsFor(250);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));
}

void RuntimeClientHandshakeTests::
    presentationSetterFailureKeepsConfirmedState() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    runtime.presentationSetterReplyMode =
        CapabilityRuntimeObject::PresentationReplyMode::Error;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QSignalSpy userMessages(&client, &RuntimeClient::userMessage);
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    const int initialGetterCalls =
        runtime.presentationGetterCalls.load(
            std::memory_order_acquire);

    client.setPresentationPreferences(
        QStringLiteral("Fahrenheit"), QStringLiteral("12H"));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationSetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !client.presentationPreferencesBusy(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(userMessages.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.presentationGetterCalls.load(
            std::memory_order_acquire) > initialGetterCalls,
        3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("24H"));
    processEventsFor(250);
    QCOMPARE(runtime.presentationSetterCalls.load(), 1);
}

void RuntimeClientHandshakeTests::
    presentationSetterFailureWaitsForReconciliation() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    runtime.presentationSetterReplyMode =
        CapabilityRuntimeObject::PresentationReplyMode::Error;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QVERIFY(service.invoke([&runtime]() {
        runtime.presentationGetterReplyMode =
            CapabilityRuntimeObject::PresentationReplyMode::Delayed;
    }));

    client.setPresentationPreferences(
        QStringLiteral("Fahrenheit"), QStringLiteral("12H"));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationSetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.presentationGetterCalls.load(
            std::memory_order_acquire),
        2, 3000);
    QVERIFY(client.presentationPreferencesBusy());
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));

    bool replySent = false;
    QVERIFY(service.invoke([&runtime, &service, &replySent]() {
        replySent = runtime.sendDelayedPresentationGetterReply(
            service.connection(), runtime.presentationPreferences);
    }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(
        !client.presentationPreferencesBusy(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));
    QCOMPARE(client.timeFormat(), QStringLiteral("24H"));
}

void RuntimeClientHandshakeTests::
    stalePresentationSignalCallbackFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Celsius"));

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    secondRuntime.presentationPreferences.temperatureUnit =
        QStringLiteral("Fahrenheit");
    secondRuntime.presentationPreferences.timeFormat =
        QStringLiteral("12H");
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));

    TryxRuntimePresentationPreferencesV1 staleSignal;
    staleSignal.revision = 99;
    staleSignal.temperatureUnit = QStringLiteral("Celsius");
    staleSignal.timeFormat = QStringLiteral("24H");

    // Model an already queued callback from owner A arriving after owner B
    // completed its handshake. A callback without its original D-Bus sender
    // must fail closed instead of borrowing the current owner field.
    client.onPresentationPreferencesChangedV1(staleSignal);

    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));
}

void RuntimeClientHandshakeTests::
    stalePresentationSetterReplyFromPreviousOwnerIsDiscarded() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    firstRuntime.presentationSetterReplyMode =
        CapabilityRuntimeObject::PresentationReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    client.setPresentationPreferences(
        QStringLiteral("Fahrenheit"), QStringLiteral("12H"));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.presentationSetterCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(client.presentationPreferencesBusy());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);
    QVERIFY(!client.presentationPreferencesReady());
    QVERIFY(!client.presentationPreferencesBusy());

    CapabilityRuntimeObject secondRuntime;
    secondRuntime.runtimeCapabilities.append(
        tryxRuntimePresentationPreferencesV1Token());
    secondRuntime.presentationPreferences.temperatureUnit =
        QStringLiteral("Fahrenheit");
    secondRuntime.presentationPreferences.timeFormat =
        QStringLiteral("12H");
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.serviceAvailable(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        client.presentationPreferencesReady(), 3000);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));

    TryxRuntimePresentationPreferencesV1 staleReply;
    staleReply.revision = 2;
    staleReply.temperatureUnit = QStringLiteral("Celsius");
    staleReply.timeFormat = QStringLiteral("24H");
    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleReply,
         &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedPresentationSetterReply(
                    firstService.connection(), staleReply);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(client.temperatureUnit(), QStringLiteral("Fahrenheit"));
    QCOMPARE(client.timeFormat(), QStringLiteral("12H"));
}

void RuntimeClientHandshakeTests::
    savedLayoutPreSendOwnerFailureIsDeliveredAsynchronously() {
    RuntimeClient client;
    client.connection_ = connectedSnapshot();
    client.display_.valid = true;
    client.display_.revision = 1;
    client.display_.deviceSerial = client.connection_.serial;

    QSignalSpy startedSpy(
        &client, &RuntimeClient::displayApplyStarted);
    QSignalSpy finishedSpy(
        &client, &RuntimeClient::displayApplyFinished);
    QSignalSpy rejectedSpy(
        &client, &RuntimeClient::operationRequestRejected);

    const QString submissionId = client.beginDisplaySubmission(
        savedLayoutRequest(), true, true, true, false, false,
        QStringLiteral("QueueSavedLayoutApplyV1"),
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), 1);

    QVERIFY(!submissionId.isEmpty());
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.constFirst().at(0).toString(), submissionId);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(rejectedSpy.count(), 0);
    QVERIFY(client.displaySubmission_.active());

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(rejectedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.constFirst().at(0).toString(), submissionId);
    QCOMPARE(finishedSpy.constFirst().at(1).toString(),
             QStringLiteral("Failed"));
    QCOMPARE(rejectedSpy.constFirst().at(0).toString(), submissionId);
    QCOMPARE(rejectedSpy.constFirst().at(1).toString(),
             QStringLiteral("SavedLayoutApply"));
    QVERIFY(!client.displaySubmission_.active());
}

void RuntimeClientHandshakeTests::
    savedLayoutsRequireExactCapabilityAndConfirmedSnapshot() {
    const TryxRuntimeSavedLayoutV1 confirmedLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Confirmed"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 confirmedSnapshot =
        readySavedLayoutsSnapshot({confirmedLayout}, 1);

    {
        CapabilityRuntimeObject runtime;
        runtime.connection = connectedSnapshot();
        runtime.connection.productId = confirmedSnapshot.productId;
        runtime.savedLayouts = confirmedSnapshot;
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());

        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.capabilitiesReady(), 3000);
        QVERIFY(!client.savedLayoutsSupported());
        QVERIFY(!client.savedLayoutsReady());
        QCOMPARE(client.savedLayoutsStatus(),
                 QStringLiteral("NotSupported"));
        QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
        QCOMPARE(runtime.savedLayoutsGetterCalls.load(), 0);
    }

    {
        CapabilityRuntimeObject runtime;
        configureSavedLayoutsRuntime(&runtime, confirmedSnapshot);
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());

        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
        QVERIFY(runtime.savedLayoutsGetterCalls.load() >= 1);
        QVERIFY(client.savedLayoutsSupported());
        QVERIFY(!client.savedLayoutsBusy());
        QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Ready"));
        QCOMPARE(client.savedLayoutsDeviceIdentity(),
                 QStringLiteral("device-a"));
        QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
        QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Confirmed")),
                 confirmedLayout.layoutId);
    }
}

void RuntimeClientHandshakeTests::
    advertisedSavedLayoutsUnknownMethodFailsClosed() {
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({}, 1));
    runtime.savedLayoutsGetterReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::UnknownMethod;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire) >= 1,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(!client.savedLayoutsBusy(), 3000);
    QVERIFY(client.savedLayoutsSupported());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QCOMPARE(client.savedLayoutsDiagnostic(),
             QStringLiteral("saved layouts method missing"));
    client.putSavedLayout(
        QStringLiteral("Must not write"), {}, savedLayoutFullDraft());
    client.deleteSavedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    processEventsFor(100);
    QCOMPARE(runtime.savedLayoutsPutCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    malformedSavedLayoutsSnapshotFailsClosed_data() {
    QTest::addColumn<TryxRuntimeSavedLayoutsSnapshotV1>("snapshot");

    TryxRuntimeSavedLayoutsSnapshotV1 invalidSchema =
        readySavedLayoutsSnapshot({}, 1);
    invalidSchema.schemaVersion = 2;
    QTest::newRow("schema-version") << invalidSchema;

    TryxRuntimeSavedLayoutV1 zeroLayoutRevision = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Zero revision"), 0);
    QTest::newRow("zero-layout-revision")
        << readySavedLayoutsSnapshot({zeroLayoutRevision}, 1);

    QTest::newRow("unsupported-product-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("device-a"),
               QStringLiteral("391a:2011"));
    QTest::newRow("legacy-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("legacy:device-a"),
               QStringLiteral("391a:1021"));
    QTest::newRow("control-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("device\ncontrol"),
               QStringLiteral("391a:1021"));
    QTest::newRow("bidi-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("device\u202Econtrol"),
               QStringLiteral("391a:1021"));
    QTest::newRow("leading-whitespace-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral(" device-a"),
               QStringLiteral("391a:1021"));
    QTest::newRow("trailing-whitespace-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("device-a "),
               QStringLiteral("391a:1021"));
    QTest::newRow("bidi-isolate-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QStringLiteral("device\u2066control"),
               QStringLiteral("391a:1021"));
    QTest::newRow("overlong-identity-with-empty-list")
        << readySavedLayoutsSnapshot(
               {}, 1, QString(257, QLatin1Char('a')),
               QStringLiteral("391a:1021"));
}

void RuntimeClientHandshakeTests::
    malformedSavedLayoutsSnapshotFailsClosed() {
    QFETCH(TryxRuntimeSavedLayoutsSnapshotV1, snapshot);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(&runtime, snapshot);
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire) >= 1,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(!client.savedLayoutsBusy(), 3000);
    QVERIFY(client.savedLayoutsSupported());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QCOMPARE(
        client.savedLayoutsDiagnostic(),
        QStringLiteral(
            "The runtime returned an invalid saved-layout snapshot"));
}

void RuntimeClientHandshakeTests::
    staleSavedLayoutsReplyFromPreviousOwnerIsDiscarded() {
    const TryxRuntimeSavedLayoutV1 staleLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("First owner"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 staleSnapshot =
        readySavedLayoutsSnapshot({staleLayout}, 1);
    CapabilityRuntimeObject firstRuntime;
    configureSavedLayoutsRuntime(&firstRuntime, staleSnapshot);
    firstRuntime.savedLayoutsGetterReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        firstRuntime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire) >= 1,
        3000);
    QVERIFY(!client.savedLayoutsReady());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    const TryxRuntimeSavedLayoutV1 currentLayout = savedLayout(
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        QStringLiteral("Second owner"), 2);
    const TryxRuntimeSavedLayoutsSnapshotV1 currentSnapshot =
        readySavedLayoutsSnapshot({currentLayout}, 2);
    CapabilityRuntimeObject secondRuntime;
    configureSavedLayoutsRuntime(&secondRuntime, currentSnapshot);
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Second owner")),
             currentLayout.layoutId);

    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &staleSnapshot,
         &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedSavedLayoutsGetterReply(
                    firstService.connection(), staleSnapshot);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QVERIFY(client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Second owner")),
             currentLayout.layoutId);
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("First owner")).isEmpty());
}

void RuntimeClientHandshakeTests::
    staleSavedLayoutPutReplyFromPreviousOwnerIsDiscarded() {
    const TryxRuntimeSavedLayoutV1 firstLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("First owner"), 1);
    CapabilityRuntimeObject firstRuntime;
    configureSavedLayoutsRuntime(
        &firstRuntime,
        readySavedLayoutsSnapshot({firstLayout}, 1));
    firstRuntime.savedLayoutsPutReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);
    QSignalSpy putFinished(
        &client, &RuntimeClient::savedLayoutPutFinished);
    client.putSavedLayout(
        QStringLiteral("Stale put"), {}, savedLayoutFullDraft());
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.savedLayoutsPutCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(client.savedLayoutsBusy());

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    const TryxRuntimeSavedLayoutV1 secondLayout = savedLayout(
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        QStringLiteral("Second owner"), 10);
    const TryxRuntimeSavedLayoutsSnapshotV1 secondSnapshot =
        readySavedLayoutsSnapshot({secondLayout}, 10);
    CapabilityRuntimeObject secondRuntime;
    configureSavedLayoutsRuntime(&secondRuntime, secondSnapshot);
    ScopedRuntimeService secondService(&secondRuntime);
    QVERIFY(secondService.start());
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Second owner")),
             secondLayout.layoutId);

    const TryxRuntimeSavedLayoutV1 stalePutLayout = savedLayout(
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        QStringLiteral("Stale put"), 2);
    const TryxRuntimeSavedLayoutsSnapshotV1 stalePutReply =
        readySavedLayoutsSnapshot({firstLayout, stalePutLayout}, 2);
    bool staleReplySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &stalePutReply,
         &staleReplySent]() {
            staleReplySent =
                firstRuntime.sendDelayedSavedLayoutsPutReply(
                    firstService.connection(), stalePutReply);
        }));
    QVERIFY(staleReplySent);
    processEventsFor(250);
    QCOMPARE(putFinished.count(), 0);
    QVERIFY(client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Second owner")),
             secondLayout.layoutId);
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Stale put")).isEmpty());
}

void RuntimeClientHandshakeTests::
    deviceIdentityChangeInvalidatesAndRequeriesSavedLayoutsWithoutMutation() {
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Device A"), 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({initialLayout}, 1));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    const int getterCallsBefore =
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire);

    const TryxRuntimeSavedLayoutsSnapshotV1 deviceBSnapshot =
        readySavedLayoutsSnapshot(
            {}, 2, QStringLiteral("device-b"));
    TryxRuntimeSnapshot deviceBConnection = connectedSnapshot(
        2, QStringLiteral("device-b"));
    deviceBConnection.productId = deviceBSnapshot.productId;
    QVERIFY(service.invoke(
        [&runtime, &deviceBSnapshot, &deviceBConnection]() {
            runtime.connection = deviceBConnection;
            runtime.savedLayouts = deviceBSnapshot;
            runtime.mediaCatalog = savedLayoutMediaCatalog(
                QStringLiteral("device-b"),
                QStringLiteral("demo.mp4"), 2);
        }));

    client.applyConnectionSnapshot(deviceBConnection);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutsPutCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire) > getterCallsBefore,
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutsDeviceIdentity(),
             QStringLiteral("device-b"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QCOMPARE(runtime.savedLayoutsPutCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    nonCanonicalConnectionIdentityCannotAuthorizeOrRetainReadySavedLayouts_data() {
    QTest::addColumn<QString>("deviceIdentity");

    QTest::newRow("leading-whitespace")
        << QStringLiteral(" device-a");
    QTest::newRow("trailing-whitespace")
        << QStringLiteral("device-a ");
    QTest::newRow("bidi-isolate")
        << QStringLiteral("device\u2066-a");
    QTest::newRow("overlong-257")
        << QString(257, QLatin1Char('a'));
}

void RuntimeClientHandshakeTests::
    nonCanonicalConnectionIdentityCannotAuthorizeOrRetainReadySavedLayouts() {
    QFETCH(QString, deviceIdentity);
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 canonicalSnapshot =
        readySavedLayoutsSnapshot({initialLayout}, 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(&runtime, canonicalSnapshot);
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    const int getterCallsBefore =
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire);

    TryxRuntimeSnapshot invalidConnection =
        connectedSnapshot(2, deviceIdentity);
    invalidConnection.productId = canonicalSnapshot.productId;
    QVERIFY(service.invoke([&runtime, &invalidConnection]() {
        runtime.connection = invalidConnection;
        runtime.savedLayoutsGetterReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    }));

    client.applyConnectionSnapshot(invalidConnection);
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire),
        getterCallsBefore + 1, 3000);

    bool replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &canonicalSnapshot, &replySent]() {
            replySent = runtime.sendDelayedSavedLayoutsGetterReply(
                service.connection(), canonicalSnapshot);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(!client.savedLayoutsBusy(), 3000);
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(),
             QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);

    client.putSavedLayout(
        QStringLiteral("Must not write"), {}, savedLayoutFullDraft());
    client.deleteSavedLayout(initialLayout.layoutId);
    QCOMPARE(client.submitSavedLayoutDraft(
                 initialLayout.layoutId, QStringLiteral("1"),
                 savedLayoutFullDraft()),
             QString());
    processEventsFor(100);
    QCOMPARE(runtime.savedLayoutsPutCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 0);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    savedLayoutPutAndDeletePublishOnlyConfirmedSnapshots() {
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({initialLayout}, 1));
    runtime.savedLayoutsPutReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    runtime.savedLayoutsDeleteReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QSignalSpy putFinished(
        &client, &RuntimeClient::savedLayoutPutFinished);
    QSignalSpy deleteFinished(
        &client, &RuntimeClient::savedLayoutDeleteFinished);

    client.putSavedLayout(
        QStringLiteral("Added"), {}, savedLayoutFullDraft());
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsPutCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(client.savedLayoutsBusy());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Added")).isEmpty());
    QCOMPARE(putFinished.count(), 0);

    quint64 putExpectedRevision = 0;
    TryxRuntimeSavedLayoutV1 submittedLayout;
    QVERIFY(service.invoke(
        [&runtime, &putExpectedRevision, &submittedLayout]() {
            putExpectedRevision =
                runtime.lastSavedLayoutsPutExpectedRevision;
            submittedLayout = runtime.lastSavedLayoutsPutLayout;
        }));
    QCOMPARE(putExpectedRevision, quint64(1));
    QCOMPARE(submittedLayout.name, QStringLiteral("Added"));
    QVERIFY(submittedLayout.layoutId.isEmpty());
    QCOMPARE(submittedLayout.revision, quint64(0));

    const TryxRuntimeSavedLayoutV1 addedLayout = savedLayout(
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        QStringLiteral("Added"), 2);
    const TryxRuntimeSavedLayoutsSnapshotV1 putConfirmed =
        readySavedLayoutsSnapshot({initialLayout, addedLayout}, 2);
    bool putReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &putConfirmed, &putReplySent]() {
            runtime.savedLayouts = putConfirmed;
            putReplySent = runtime.sendDelayedSavedLayoutsPutReply(
                service.connection(), putConfirmed);
        }));
    QVERIFY(putReplySent);
    QTRY_COMPARE_WITH_TIMEOUT(putFinished.count(), 1, 3000);
    QVERIFY(putFinished.constFirst().at(3).toBool());
    QCOMPARE(putFinished.constFirst().at(1).toString(),
             addedLayout.layoutId);
    QCOMPARE(putFinished.constFirst().at(2).toString(),
             QStringLiteral("2"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 2);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Added")),
             addedLayout.layoutId);
    TryxRuntimeSavedLayoutV1 publishedAddedLayout;
    QVERIFY(client.savedLayoutModel()->layoutById(
        addedLayout.layoutId, &publishedAddedLayout));
    QCOMPARE(publishedAddedLayout, addedLayout);

    client.deleteSavedLayout(initialLayout.layoutId);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsDeleteCalls.load(
            std::memory_order_acquire),
        1, 3000);
    QVERIFY(client.savedLayoutsBusy());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 2);
    QCOMPARE(deleteFinished.count(), 0);

    quint64 deleteExpectedRevision = 0;
    QString deletedLayoutId;
    QVERIFY(service.invoke(
        [&runtime, &deleteExpectedRevision, &deletedLayoutId]() {
            deleteExpectedRevision =
                runtime.lastSavedLayoutsDeleteExpectedRevision;
            deletedLayoutId = runtime.lastSavedLayoutsDeleteLayoutId;
        }));
    QCOMPARE(deleteExpectedRevision, quint64(2));
    QCOMPARE(deletedLayoutId, initialLayout.layoutId);

    const TryxRuntimeSavedLayoutsSnapshotV1 deleteConfirmed =
        readySavedLayoutsSnapshot({addedLayout}, 3);
    bool deleteReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &deleteConfirmed, &deleteReplySent]() {
            runtime.savedLayouts = deleteConfirmed;
            deleteReplySent =
                runtime.sendDelayedSavedLayoutsDeleteReply(
                    service.connection(), deleteConfirmed);
        }));
    QVERIFY(deleteReplySent);
    QTRY_COMPARE_WITH_TIMEOUT(deleteFinished.count(), 1, 3000);
    QVERIFY(deleteFinished.constFirst().at(1).toBool());
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QVERIFY(!client.savedLayoutModel()->layoutById(
        initialLayout.layoutId, nullptr));
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Initial")).isEmpty());
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Added")),
             addedLayout.layoutId);
}

void RuntimeClientHandshakeTests::
    savedLayoutMutationRepliesRequireExactNextRevision_data() {
    QTest::addColumn<QString>("operation");
    QTest::addColumn<quint64>("responseRevision");
    QTest::addColumn<bool>("changePutContent");

    QTest::newRow("put-revision-jump")
        << QStringLiteral("put") << quint64(3) << false;
    QTest::newRow("put-content-mismatch")
        << QStringLiteral("put") << quint64(2) << true;
    QTest::newRow("delete-revision-jump")
        << QStringLiteral("delete") << quint64(3) << false;
}

void RuntimeClientHandshakeTests::
    savedLayoutMutationRepliesRequireExactNextRevision() {
    QFETCH(QString, operation);
    QFETCH(quint64, responseRevision);
    QFETCH(bool, changePutContent);

    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({initialLayout}, 1));
    runtime.savedLayoutsPutReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    runtime.savedLayoutsDeleteReplyMode =
        CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);

    QSignalSpy putFinished(
        &client, &RuntimeClient::savedLayoutPutFinished);
    QSignalSpy deleteFinished(
        &client, &RuntimeClient::savedLayoutDeleteFinished);
    bool replySent = false;
    if (operation == QStringLiteral("put")) {
        client.putSavedLayout(
            QStringLiteral("Added"), {}, savedLayoutFullDraft());
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.savedLayoutsPutCalls.load(
                std::memory_order_acquire),
            1, 3000);

        TryxRuntimeSavedLayoutV1 addedLayout = savedLayout(
            QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
            QStringLiteral("Added"), responseRevision);
        if (changePutContent) {
            addedLayout.request.display.brightness = 51;
        }
        const TryxRuntimeSavedLayoutsSnapshotV1 response =
            readySavedLayoutsSnapshot(
                {initialLayout, addedLayout}, responseRevision);
        QVERIFY(service.invoke(
            [&runtime, &service, &response, &replySent]() {
                replySent = runtime.sendDelayedSavedLayoutsPutReply(
                    service.connection(), response);
            }));
        QVERIFY(replySent);
        QTRY_COMPARE_WITH_TIMEOUT(putFinished.count(), 1, 3000);
        QVERIFY(!putFinished.constFirst().at(3).toBool());
    } else {
        QCOMPARE(operation, QStringLiteral("delete"));
        client.deleteSavedLayout(initialLayout.layoutId);
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.savedLayoutsDeleteCalls.load(
                std::memory_order_acquire),
            1, 3000);
        const TryxRuntimeSavedLayoutsSnapshotV1 response =
            readySavedLayoutsSnapshot({}, responseRevision);
        QVERIFY(service.invoke(
            [&runtime, &service, &response, &replySent]() {
                replySent = runtime.sendDelayedSavedLayoutsDeleteReply(
                    service.connection(), response);
            }));
        QVERIFY(replySent);
        QTRY_COMPARE_WITH_TIMEOUT(deleteFinished.count(), 1, 3000);
        QVERIFY(!deleteFinished.constFirst().at(1).toBool());
    }

    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
}

void RuntimeClientHandshakeTests::
    savedLayoutReadsRejectSameRevisionChangesAndRollback() {
    const TryxRuntimeSavedLayoutV1 confirmedLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Confirmed at two"), 2);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({confirmedLayout}, 2));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutIdForName(
                 QStringLiteral("Confirmed at two")),
             confirmedLayout.layoutId);
    int getterCalls = runtime.savedLayoutsGetterCalls.load(
        std::memory_order_acquire);

    const TryxRuntimeSavedLayoutV1 changedAtSameRevision = savedLayout(
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        QStringLiteral("Changed bytes"), 2);
    const TryxRuntimeSavedLayoutsSnapshotV1 sameRevisionChanged =
        readySavedLayoutsSnapshot({changedAtSameRevision}, 2);
    QVERIFY(service.invoke([&runtime]() {
        runtime.savedLayoutsGetterReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
    }));
    client.refreshSavedLayouts();
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire),
        getterCalls + 1, 3000);
    bool replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &sameRevisionChanged, &replySent]() {
            replySent =
                runtime.sendDelayedSavedLayoutsGetterReply(
                    service.connection(), sameRevisionChanged);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(!client.savedLayoutsBusy(), 3000);
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);

    getterCalls = runtime.savedLayoutsGetterCalls.load(
        std::memory_order_acquire);
    const TryxRuntimeSavedLayoutV1 rolledBackLayout = savedLayout(
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        QStringLiteral("Rolled back"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 rolledBack =
        readySavedLayoutsSnapshot({rolledBackLayout}, 1);
    client.refreshSavedLayouts();
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire),
        getterCalls + 1, 3000);
    replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &rolledBack, &replySent]() {
            replySent =
                runtime.sendDelayedSavedLayoutsGetterReply(
                    service.connection(), rolledBack);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(!client.savedLayoutsBusy(), 3000);
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
}

void RuntimeClientHandshakeTests::
    savedLayoutConflictReconcilesAndCommitUnknownFailsClosed() {
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(
        &runtime, readySavedLayoutsSnapshot({initialLayout}, 1));
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);
    const int getterCallsBeforeConflict =
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire);
    QVERIFY(getterCallsBeforeConflict >= 1);
    QSignalSpy putFinished(
        &client, &RuntimeClient::savedLayoutPutFinished);
    QSignalSpy deleteFinished(
        &client, &RuntimeClient::savedLayoutDeleteFinished);

    QVERIFY(service.invoke([&runtime]() {
        runtime.savedLayoutsPutReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Error;
        runtime.savedLayoutsPutErrorName = QStringLiteral(
            "org.tryx.Panorama.Error.InvalidSavedLayout");
        runtime.savedLayoutsPutErrorMessage =
            QStringLiteral("layout was rejected");
    }));
    client.putSavedLayout(
        QStringLiteral("Rejected deterministic"), {},
        savedLayoutFullDraft());
    QTRY_COMPARE_WITH_TIMEOUT(putFinished.count(), 1, 3000);
    QVERIFY(!putFinished.constFirst().at(3).toBool());
    QVERIFY(client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Ready"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Initial")),
             initialLayout.layoutId);
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Rejected deterministic")).isEmpty());
    processEventsFor(100);
    QCOMPARE(runtime.savedLayoutsGetterCalls.load(),
             getterCallsBeforeConflict);
    putFinished.clear();

    const TryxRuntimeSavedLayoutV1 serverWinner = savedLayout(
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        QStringLiteral("Server winner"), 2);
    const TryxRuntimeSavedLayoutsSnapshotV1 reconciled =
        readySavedLayoutsSnapshot({serverWinner}, 2);
    QVERIFY(service.invoke([&runtime, &reconciled]() {
        runtime.savedLayouts = reconciled;
        runtime.savedLayoutsGetterReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
        runtime.savedLayoutsPutReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Error;
        runtime.savedLayoutsPutErrorName = QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict");
        runtime.savedLayoutsPutErrorMessage =
            QStringLiteral("server revision advanced");
    }));

    client.putSavedLayout(
        QStringLiteral("Conflict candidate"), {},
        savedLayoutFullDraft());
    QTRY_COMPARE_WITH_TIMEOUT(putFinished.count(), 1, 3000);
    QVERIFY(!putFinished.constFirst().at(3).toBool());
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Conflict candidate")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire),
        getterCallsBeforeConflict + 1, 3000);
    QVERIFY(client.savedLayoutsBusy());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QVERIFY(client.savedLayoutIdForName(
                QStringLiteral("Initial")).isEmpty());

    bool reconciliationReplySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &reconciled,
         &reconciliationReplySent]() {
            reconciliationReplySent =
                runtime.sendDelayedSavedLayoutsGetterReply(
                    service.connection(), reconciled);
        }));
    QVERIFY(reconciliationReplySent);
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QCOMPARE(client.savedLayoutIdForName(
                 QStringLiteral("Server winner")),
             serverWinner.layoutId);

    QVERIFY(service.invoke([&runtime]() {
        runtime.savedLayoutsDeleteReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Error;
        runtime.savedLayoutsDeleteErrorName = QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsCommitUnknown");
        runtime.savedLayoutsDeleteErrorMessage =
            QStringLiteral("commit outcome unknown");
    }));
    const int getterCallsBeforeCommitUnknown =
        runtime.savedLayoutsGetterCalls.load(
            std::memory_order_acquire);
    client.deleteSavedLayout(serverWinner.layoutId);
    QTRY_COMPARE_WITH_TIMEOUT(deleteFinished.count(), 1, 3000);
    QVERIFY(!deleteFinished.constFirst().at(1).toBool());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutsDiagnostic(),
             QStringLiteral("commit outcome unknown"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    processEventsFor(250);
    QCOMPARE(runtime.savedLayoutsGetterCalls.load(),
             getterCallsBeforeCommitUnknown);

    const int deleteCallsBeforeRejectedRetry =
        runtime.savedLayoutsDeleteCalls.load();
    const int putCallsBeforeRejectedRetry =
        runtime.savedLayoutsPutCalls.load();
    client.deleteSavedLayout(serverWinner.layoutId);
    client.putSavedLayout(
        QStringLiteral("Rejected retry"), {},
        savedLayoutFullDraft());
    processEventsFor(100);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(),
             deleteCallsBeforeRejectedRetry);
    QCOMPARE(runtime.savedLayoutsPutCalls.load(),
             putCallsBeforeRejectedRetry);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    savedLayoutMutationsRequireCurrentOwnerBeforeSend() {
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        readySavedLayoutsSnapshot({initialLayout}, 1);

    {
        CapabilityRuntimeObject runtime;
        configureSavedLayoutsRuntime(&runtime, snapshot);
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());

        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);
        QSignalSpy finished(
            &client, &RuntimeClient::savedLayoutPutFinished);

        QVERIFY(service.releaseServiceName());
        QVERIFY(client.savedLayoutsReady());
        client.putSavedLayout(
            QStringLiteral("Must not dispatch"), {},
            savedLayoutFullDraft());

        QCOMPARE(runtime.savedLayoutsPutCalls.load(), 0);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.constFirst().at(3).toBool());
        QVERIFY(finished.constFirst().at(4).toString().contains(
            QStringLiteral("changed before"), Qt::CaseInsensitive));
        QVERIFY(!client.savedLayoutsReady());
        QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    }

    {
        CapabilityRuntimeObject runtime;
        configureSavedLayoutsRuntime(&runtime, snapshot);
        ScopedRuntimeService service(&runtime);
        QVERIFY(service.start());

        RuntimeClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
        QSignalSpy finished(
            &client, &RuntimeClient::savedLayoutDeleteFinished);

        QVERIFY(service.releaseServiceName());
        QVERIFY(client.savedLayoutsReady());
        client.deleteSavedLayout(initialLayout.layoutId);

        QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 0);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.constFirst().at(1).toBool());
        QVERIFY(finished.constFirst().at(2).toString().contains(
            QStringLiteral("changed before"), Qt::CaseInsensitive));
        QVERIFY(!client.savedLayoutsReady());
        QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    }
}

void RuntimeClientHandshakeTests::
    savedLayoutTransportAmbiguityFailsClosedAndReconciles() {
    const TryxRuntimeSavedLayoutV1 initialLayout = savedLayout(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        QStringLiteral("Initial"), 1);
    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        readySavedLayoutsSnapshot({initialLayout}, 1);
    CapabilityRuntimeObject runtime;
    configureSavedLayoutsRuntime(&runtime, snapshot);
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.metricsCatalogReady(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(client.mediaModel()->rowCount(), 1, 3000);
    QSignalSpy putFinished(
        &client, &RuntimeClient::savedLayoutPutFinished);
    QSignalSpy deleteFinished(
        &client, &RuntimeClient::savedLayoutDeleteFinished);

    int getterCalls = runtime.savedLayoutsGetterCalls.load(
        std::memory_order_acquire);
    QVERIFY(service.invoke([&runtime]() {
        runtime.savedLayoutsGetterReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Delayed;
        runtime.savedLayoutsPutReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Error;
        runtime.savedLayoutsPutErrorName = QStringLiteral(
            "org.freedesktop.DBus.Error.NoReply");
        runtime.savedLayoutsPutErrorMessage = QStringLiteral(
            "put reply was lost");
    }));

    client.putSavedLayout(
        QStringLiteral("Ambiguous put"), {}, savedLayoutFullDraft());
    QTRY_COMPARE_WITH_TIMEOUT(putFinished.count(), 1, 3000);
    QVERIFY(!putFinished.constFirst().at(3).toBool());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(std::memory_order_acquire),
        getterCalls + 1, 3000);
    QVERIFY(client.savedLayoutsBusy());

    TryxRuntimeSavedLayoutV1 committedPut;
    QVERIFY(service.invoke([&runtime, &committedPut]() {
        committedPut = runtime.lastSavedLayoutsPutLayout;
    }));
    QVERIFY(committedPut.layoutId.isEmpty());
    committedPut.layoutId = QStringLiteral(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    committedPut.revision = 2;
    const TryxRuntimeSavedLayoutsSnapshotV1 committedPutSnapshot =
        readySavedLayoutsSnapshot({initialLayout, committedPut}, 2);

    bool replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &committedPutSnapshot, &replySent]() {
            replySent = runtime.sendDelayedSavedLayoutsGetterReply(
                service.connection(), committedPutSnapshot);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 2);
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Ambiguous put")),
             committedPut.layoutId);

    getterCalls = runtime.savedLayoutsGetterCalls.load(
        std::memory_order_acquire);
    QVERIFY(service.invoke([&runtime]() {
        runtime.savedLayoutsDeleteReplyMode =
            CapabilityRuntimeObject::SavedLayoutsReplyMode::Error;
        runtime.savedLayoutsDeleteErrorName = QStringLiteral(
            "org.freedesktop.DBus.Error.Disconnected");
        runtime.savedLayoutsDeleteErrorMessage = QStringLiteral(
            "delete reply was lost");
    }));

    client.deleteSavedLayout(initialLayout.layoutId);
    QTRY_COMPARE_WITH_TIMEOUT(deleteFinished.count(), 1, 3000);
    QVERIFY(!deleteFinished.constFirst().at(1).toBool());
    QVERIFY(!client.savedLayoutsReady());
    QCOMPARE(client.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(client.savedLayoutModel()->rowCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.savedLayoutsGetterCalls.load(std::memory_order_acquire),
        getterCalls + 1, 3000);
    QVERIFY(client.savedLayoutsBusy());

    const TryxRuntimeSavedLayoutsSnapshotV1 committedDeleteSnapshot =
        readySavedLayoutsSnapshot({committedPut}, 3);
    replySent = false;
    QVERIFY(service.invoke(
        [&runtime, &service, &committedDeleteSnapshot, &replySent]() {
            replySent = runtime.sendDelayedSavedLayoutsGetterReply(
                service.connection(), committedDeleteSnapshot);
        }));
    QVERIFY(replySent);
    QTRY_VERIFY_WITH_TIMEOUT(client.savedLayoutsReady(), 3000);
    QCOMPARE(client.savedLayoutModel()->rowCount(), 1);
    QVERIFY(client.savedLayoutIdForName(QStringLiteral("Initial")).isEmpty());
    QCOMPARE(client.savedLayoutIdForName(QStringLiteral("Ambiguous put")),
             committedPut.layoutId);
    QCOMPARE(runtime.savedLayoutsPutCalls.load(), 1);
    QCOMPARE(runtime.savedLayoutsDeleteCalls.load(), 1);
    QCOMPARE(runtime.savedLayoutsQueueCalls.load(), 0);
}

void RuntimeClientHandshakeTests::
    cacheCleanupNoReplyReconcilesOnceAndCancelDoesNotReplay() {
    CapabilityRuntimeObject runtime;
    runtime.runtimeCapabilities.append(
        tryxRuntimeCacheCleanupV1Token());
    runtime.cacheCleanupQueueReplyMode =
        CapabilityRuntimeObject::CacheCleanupQueueReplyMode::NoReply;
    runtime.cacheCleanupCancelNoReply = true;
    ScopedRuntimeService service(&runtime);
    QVERIFY(service.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeCacheCleanupV1Token()),
        3000);
    QSignalSpy accepted(
        &client, &RuntimeClient::operationRequestAccepted);
    QSignalSpy rejected(
        &client, &RuntimeClient::operationRequestRejected);
    QSignalSpy observed(
        &client, &RuntimeClient::operationUpdated);
    QSignalSpy refreshResolved(
        &client, &RuntimeClient::cacheCleanupRefreshResolved);
    QSignalSpy refreshFailed(
        &client, &RuntimeClient::cacheCleanupRefreshFailed);

    const QString operationId = client.queueCacheCleanup();
    QVERIFY(!operationId.isEmpty());
    QCOMPARE(
        QUuid(operationId).toString(QUuid::WithoutBraces),
        operationId);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.cacheCleanupQueueCalls.load(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.cacheCleanupGetCalls.load(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 3000);
    QCOMPARE(rejected.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(observed.count(), 1, 3000);
    const TryxRuntimeOperationInfo reconciled =
        observed.constFirst().constFirst()
            .value<TryxRuntimeOperationInfo>();
    QCOMPARE(reconciled.id, operationId);
    QCOMPARE(reconciled.kind, QStringLiteral("CacheCleanup"));

    QVERIFY(service.invoke([&runtime]() {
        runtime.cacheCleanupOperation.state =
            QStringLiteral("Succeeded");
        runtime.cacheCleanupOperation.stage =
            QStringLiteral("Succeeded");
        runtime.cacheCleanupOperation.terminalOutcome =
            QStringLiteral("Succeeded");
        runtime.cacheCleanupOperation.completed = 0;
        runtime.cacheCleanupOperation.total = 0;
        runtime.cacheCleanupOperation.confirmedBytes = 0;
    }));
    QVERIFY(client.refreshCacheCleanup(operationId));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.cacheCleanupGetCalls.load(), 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(refreshResolved.count(), 1, 3000);
    QCOMPARE(refreshFailed.count(), 0);
    const TryxRuntimeOperationInfo refreshed =
        refreshResolved.constFirst().constFirst()
            .value<TryxRuntimeOperationInfo>();
    QCOMPARE(refreshed.id, operationId);
    QCOMPARE(refreshed.kind, QStringLiteral("CacheCleanup"));
    QCOMPARE(refreshed.state, QStringLiteral("Succeeded"));

    QVERIFY(client.cancelCacheCleanup(operationId));
    QTRY_COMPARE_WITH_TIMEOUT(
        runtime.cacheCleanupCancelCalls.load(), 1, 3000);
    processEventsFor(250);
    QCOMPARE(runtime.cacheCleanupCancelCalls.load(), 1);
    QCOMPARE(runtime.cacheCleanupGetCalls.load(), 2);
    QCOMPARE(runtime.cacheCleanupQueueCalls.load(), 1);
}

void RuntimeClientHandshakeTests::
    cacheCleanupOwnerReplacementNeverTargetsNewOwner() {
    CapabilityRuntimeObject firstRuntime;
    firstRuntime.runtimeCapabilities.append(
        tryxRuntimeCacheCleanupV1Token());
    firstRuntime.cacheCleanupQueueReplyMode =
        CapabilityRuntimeObject::CacheCleanupQueueReplyMode::Delayed;
    ScopedRuntimeService firstService(&firstRuntime);
    QVERIFY(firstService.start());

    RuntimeClient client;
    QTRY_VERIFY_WITH_TIMEOUT(
        client.hasRuntimeCapability(
            tryxRuntimeCacheCleanupV1Token()),
        3000);
    QSignalSpy rejected(
        &client, &RuntimeClient::operationRequestRejected);
    QSignalSpy failed(
        &client, &RuntimeClient::operationRequestFailed);
    const QString operationId = client.queueCacheCleanup();
    QVERIFY(!operationId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(
        firstRuntime.cacheCleanupQueueCalls.load(), 1, 3000);
    const QString firstOwner = client.cacheCleanupOwner_;
    QVERIFY(firstOwner.startsWith(QLatin1Char(':')));

    QVERIFY(firstService.releaseServiceName());
    QTRY_VERIFY_WITH_TIMEOUT(!client.serviceAvailable(), 3000);

    CapabilityRuntimeObject replacementRuntime;
    replacementRuntime.runtimeCapabilities.append(
        tryxRuntimeCacheCleanupV1Token());
    ScopedRuntimeService replacementService(&replacementRuntime);
    QVERIFY(replacementService.start());
    QTRY_VERIFY_WITH_TIMEOUT(
        client.serviceAvailable() && client.capabilitiesReady() &&
            client.runtimeOwner_ != firstOwner,
        3000);

    QVERIFY(!client.cancelCacheCleanup(operationId));
    QVERIFY(!client.refreshCacheCleanup(operationId));
    QCOMPARE(replacementRuntime.cacheCleanupCancelCalls.load(), 0);
    QCOMPARE(replacementRuntime.cacheCleanupGetCalls.load(), 0);
    QCOMPARE(replacementRuntime.cacheCleanupQueueCalls.load(), 0);

    bool replySent = false;
    QVERIFY(firstService.invoke(
        [&firstRuntime, &firstService, &replySent]() {
            replySent =
                firstRuntime.sendDelayedCacheCleanupQueueReply(
                    firstService.connection());
        }));
    QVERIFY(replySent);
    QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
    QCOMPARE(failed.constFirst().at(0).toString(), operationId);
    QCOMPARE(failed.constFirst().at(1).toString(),
             QStringLiteral("CacheCleanup"));
    QVERIFY(failed.constFirst().at(3).toBool());
    processEventsFor(250);
    QCOMPARE(firstRuntime.cacheCleanupQueueCalls.load(), 1);
    QCOMPARE(firstRuntime.cacheCleanupGetCalls.load(), 0);
    QCOMPARE(firstRuntime.cacheCleanupCancelCalls.load(), 0);
    QCOMPARE(replacementRuntime.cacheCleanupQueueCalls.load(), 0);
    QCOMPARE(replacementRuntime.cacheCleanupGetCalls.load(), 0);
    QCOMPARE(replacementRuntime.cacheCleanupCancelCalls.load(), 0);
    QVERIFY(client.queueCacheCleanup().isEmpty());
}

QTEST_GUILESS_MAIN(RuntimeClientHandshakeTests)

#include "runtimeclient_handshake_tests.moc"
