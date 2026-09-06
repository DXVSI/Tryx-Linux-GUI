#include "devicemanager.h"
#include "configurationformatbackup.h"
#include "printerlifecycle_p.h"
#include "deleteintentstore.h"
#include "devicemediaartifactstore.h"
#include "devicemanagermessages.h"
#include "mediacatalogstore.h"
#include "mediatransform.h"
#include "paseoverlayconfig.h"
#include "pasemetricsconfigstore.h"
#include "privateruntimepaths.h"
#include "printermediafileintegrity.h"
#include "printermediaidentity.h"
#include "printermediapreparer.h"
#include "printermediavalidator.h"
#include "printerprotocol.h"
#include "runtimeapplyrequestcodec.h"
#include "runtimedowngradestore.h"
#include "runtimepresentationpreferencesstore.h"
#include "savedlayoutstore.h"
#include "supportsnapshot.h"
#include "systemmonitor.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <utility>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using namespace tryx::printer_lifecycle;

using tryx::printer_media_file_integrity::isSha256Hex;
using tryx::printer_media_file_integrity::sha256File;
using tryx::printer_media_file_integrity::sourceFingerprint;
using tryx::private_runtime_paths::ensurePrivateDirectory;
using tryx::printer_media_identity::generatedPrinterMediaName;
using tryx::printer_media_identity::h264PrinterNameForConversion;
using tryx::printer_media_identity::printerConversionProfile;
using tryx::printer_media_identity::printerConversionProfileMatchesProduct;
using tryx::printer_media_identity::paseRecoveredConversionProfile;
using tryx::printer_media_identity::printerMediaConversionIdentity;
using tryx::printer_media_identity::printerMediaNameMatchesProfile;
using tryx::printer_media_validator::RecoveredH264ProbeMetadata;
using tryx::printer_media_validator::validateRecoveredH264;
using tryx::pase_overlay_config::hasDuplicateMetricLabels;
using tryx::pase_overlay_config::hasDuplicateValues;
using tryx::pase_overlay_config::isSupportedPaseBadge;
using tryx::pase_overlay_config::isSupportedPaseMetricLabel;
using tryx::pase_overlay_config::isValidPaseTextColor;
using tryx::pase_overlay_config::normalizeAndValidatePaseApplyOverlayStyles;
using tryx::pase_overlay_config::paseOverlayFromApplyRequest;
using tryx::pase_overlay_config::paseOverlayFromMetricsRequest;
using tryx::pase_overlay_config::paseOverlayHasContent;
using tryx::pase_overlay_config::paseOverlayHasMetrics;
using tryx::pase_overlay_config::paseOverlayRequestsBadge;
using tryx::pase_overlay_config::paseTextColorName;
using tryx::pase_overlay_config::paseUploadApplyRequestIsValid;
using tryx::pase_overlay_config::printerMediaConfigName;
using tryx::pase_overlay_config::printerPresetMediaFile;
using tryx::runtime_apply_request_codec::runtimeApplyRequestFingerprint;
using tryx::runtime_apply_request_codec::runtimeApplyRequestFromJson;
using tryx::runtime_apply_request_codec::runtimeApplyRequestToJson;
using tryx::runtime_apply_request_codec::runtimeMediaTransformRequestFingerprint;

QString savedLayoutStoreErrorName(
    tryx::SavedLayoutStore::ErrorCode code) {
    using ErrorCode = tryx::SavedLayoutStore::ErrorCode;
    switch (code) {
    case ErrorCode::None:
        return {};
    case ErrorCode::RevisionConflict:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsRevisionConflict");
    case ErrorCode::NameConflict:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutNameConflict");
    case ErrorCode::CommitUnknown:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsCommitUnknown");
    case ErrorCode::InvalidInput:
    case ErrorCode::ResourceLimitExceeded:
    case ErrorCode::NotFound:
        return QStringLiteral(
            "org.tryx.Panorama.Error.InvalidSavedLayout");
    case ErrorCode::WritesDisabled:
    case ErrorCode::DirectoryUnavailable:
    case ErrorCode::UnsafePath:
    case ErrorCode::WriteFailed:
    case ErrorCode::CommitFailed:
        return QStringLiteral(
            "org.tryx.Panorama.Error.SavedLayoutsUnavailable");
    }
    return QStringLiteral(
        "org.tryx.Panorama.Error.SavedLayoutsUnavailable");
}

constexpr qint64 kMaxRetryCacheBytes =
    tryx::printer_media_file_integrity::kMaximumPreparedMediaBytes;
constexpr qint64 kMaxThumbnailBytes =
    tryx::printer_media_file_integrity::kMaximumThumbnailBytes;
constexpr auto kRetryCacheTransitionConflictId =
    "retry-cache-transition-conflict";
constexpr qint64 kFileTransmitChunkSize = 0x40000;
constexpr int kDeviceMediaSweepIntervalMs = 5000;
constexpr quint16 kTurrisProductId = 0x2011;

}  // namespace

// --- DeviceManager ---

DeviceManager::DeviceManager(QObject *parent)
    : DeviceManager(new PrinterDeviceMonitor, true, parent) {}

void DeviceManager::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    sessionController_.setOverlayLeaseMode(mode);
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

bool DeviceManager::setPresentationPreferences(
    quint64 expectedRevision, const QString &temperatureUnit,
    const QString &timeFormat,
    TryxRuntimePresentationPreferencesV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    if (confirmed) {
        *confirmed = presentationPreferences_;
    }
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = presentationPreferences_;
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };

    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradeV10Prepared"),
            tr("Device mutations are blocked because runtime downgrade preparation is committed"));
    }

    if (!tryxTemperatureUnitIsValid(temperatureUnit) ||
        !tryxTimeFormatIsValid(timeFormat)) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidPresentationPreferences"),
            tr("The presentation preferences are invalid"));
    }
    if (temperatureUnit == presentationPreferences_.temperatureUnit &&
        timeFormat == presentationPreferences_.timeFormat) {
        return true;
    }
    if (expectedRevision != presentationPreferences_.revision) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesConflict"),
            tr("The presentation preferences changed; refresh them before saving again"));
    }
    if (presentationPreferences_.revision ==
        std::numeric_limits<quint64>::max()) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesConflict"),
            tr("The presentation preference revision is exhausted; restart the runtime before saving again"));
    }
    if (!runtimePresentationPreferencesStore_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesPersistenceFailed"),
            tr("The presentation preferences store is unavailable"));
    }
    const auto persisted = runtimePresentationPreferencesStore_->persist(
        temperatureUnit, timeFormat);
    if (!persisted.ok()) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.PresentationPreferencesPersistenceFailed"),
            persisted.detail.isEmpty()
                ? tr("The presentation preferences could not be saved")
                : persisted.detail);
    }

    presentationPreferences_.temperatureUnit = temperatureUnit;
    presentationPreferences_.timeFormat = timeFormat;
    ++presentationPreferences_.revision;
    const TryxRuntimePresentationPreferencesV1 published =
        presentationPreferences_;
    worker_->publishPresentationPreferences(published);
    if (worker_->thread() == QThread::currentThread()) {
        worker_->applyPublishedPresentationPreferences();
    } else {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() {
                worker->applyPublishedPresentationPreferences();
            },
            Qt::QueuedConnection);
    }
    if (confirmed) {
        *confirmed = published;
    }
    emit presentationPreferencesChanged(published);
    return true;
}

TryxRuntimeSavedLayoutsSnapshotV1
DeviceManager::savedLayoutsSnapshot() const {
    const auto current = savedLayoutsSnapshotV2();
    TryxRuntimeSavedLayoutsSnapshotV1 snapshot{1, current.revision, current.status,
        current.diagnostic, current.deviceIdentity, current.productId, {}};
    for (const auto &layout : current.layouts) {
        TryxRuntimeSavedLayoutV1 legacy;
        if (tryxSavedLayoutV2ToV1(layout, &legacy)) snapshot.layouts.append(legacy);
    }
    return snapshot;
}

TryxRuntimeSavedLayoutsSnapshotV2 DeviceManager::savedLayoutsSnapshotV2() const {
    TryxRuntimeSavedLayoutsSnapshotV2 snapshot;
    snapshot.revision = savedLayoutStore_
        ? savedLayoutStore_->revision()
        : 0;
    if (!sessionController_.state().connected ||
        !sessionController_.state().printerClassConnected) {
        snapshot.status = QStringLiteral("Disconnected");
        return snapshot;
    }

    snapshot.deviceIdentity = sessionController_.state().printerDeviceSerial;
    snapshot.productId =
        printerProductIdString(sessionController_.state().printerProductId);
    const auto profile = currentPrinterProductProfile();
    if (snapshot.deviceIdentity.isEmpty()) {
        snapshot.status = QStringLiteral("Disconnected");
        snapshot.productId.clear();
        return snapshot;
    }
    if (!tryxSavedLayoutDeviceIdentityIsCanonical(
            snapshot.deviceIdentity)) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = tr(
            "The saved layout device identity is invalid");
        return snapshot;
    }
    if (!profile ||
        (profile->productId != 0x1011 && profile->productId != 0x1021) ||
        !profile->mediaCatalogSupported ||
        !profile->displayConfigurationSupported ||
        !profile->overlayMetricsSupported) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }
    if (!savedLayoutStore_ || !savedLayoutsStoreLoaded_ ||
        !savedLayoutStore_->writesEnabled()) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = savedLayoutsFailureDetail_.isEmpty()
            ? tr("Saved layouts are unavailable")
            : savedLayoutsFailureDetail_.left(512);
        return snapshot;
    }

    QString exactIdentity;
    QString exactProduct;
    if (!currentSavedLayoutsContext(
            &exactIdentity, &exactProduct)) {
        snapshot.status = QStringLiteral("Unavailable");
        snapshot.diagnostic = tr(
            "Saved layouts are waiting for the current device handshake");
        return snapshot;
    }
    return savedLayoutStore_->snapshotV2(exactIdentity, exactProduct);
}

bool DeviceManager::putSavedLayout(
    quint64 expectedSnapshotRevision,
    const TryxRuntimeSavedLayoutV1 &layout,
    TryxRuntimeSavedLayoutsSnapshotV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    const bool ok = putSavedLayoutInternal(expectedSnapshotRevision, tryxSavedLayoutV2FromV1(layout),
        true, nullptr, errorName, errorMessage);
    if (confirmed) *confirmed = savedLayoutsSnapshot();
    return ok;
}

bool DeviceManager::putSavedLayoutV2(quint64 expectedSnapshotRevision, const TryxRuntimeSavedLayoutV2 &layout,
    TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage) {
    return putSavedLayoutInternal(expectedSnapshotRevision, layout, false, confirmed, errorName, errorMessage);
}

bool DeviceManager::putSavedLayoutInternal(quint64 expectedSnapshotRevision, const TryxRuntimeSavedLayoutV2 &layout,
    bool legacyInterface, TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = savedLayoutsSnapshotV2();
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };

    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradePrepared"),
            tr("Saved layouts are locked after runtime downgrade preparation"));
    }
    const TryxRuntimeSavedLayoutsSnapshotV2 current = savedLayoutsSnapshotV2();
    if (current.status != QStringLiteral("Ready")) {
        return fail(
            current.status == QStringLiteral("Unsupported")
                ? QStringLiteral(
                      "org.tryx.Panorama.Error.UnsupportedProduct")
                : QStringLiteral(
                      "org.tryx.Panorama.Error.SavedLayoutsUnavailable"),
            current.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : current.diagnostic);
    }
    if (layout.deviceIdentity != current.deviceIdentity ||
        layout.productId != current.productId) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidSavedLayout"),
            tr("The saved layout belongs to another device"));
    }
    QList<TryxRuntimeSavedMediaRefV1> proof;
    QString validationError;
    if (!buildSavedLayoutMediaProof(
            layout.request, &proof, &validationError) ||
        proof != layout.media) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.InvalidSavedLayout"),
            validationError.isEmpty()
                ? tr("The saved layout media does not match the current catalog")
                : validationError);
    }

    TryxRuntimeSavedLayoutV1 legacy;
    if (legacyInterface && !tryxSavedLayoutV2ToV1(layout, &legacy))
        return fail(QStringLiteral("org.tryx.Panorama.Error.InvalidSavedLayout"), tr("The saved layout is invalid"));
    const tryx::SavedLayoutStore::MutationResult result = legacyInterface
        ? savedLayoutStore_->put(expectedSnapshotRevision, legacy)
        : savedLayoutStore_->putV2(expectedSnapshotRevision, layout);
    if (!result.ok()) {
        if (result.commitMayExist ||
            result.code ==
                tryx::SavedLayoutStore::ErrorCode::CommitUnknown) {
            savedLayoutsFailureDetail_ = result.detail.left(512);
        }
        return fail(
            savedLayoutStoreErrorName(result.code),
            result.detail.isEmpty()
                ? tr("The saved layout could not be stored")
                : result.detail);
    }
    if (confirmed) {
        *confirmed = savedLayoutsSnapshotV2();
    }
    return true;
}

bool DeviceManager::deleteSavedLayout(
    quint64 expectedSnapshotRevision, const QString &layoutId,
    TryxRuntimeSavedLayoutsSnapshotV1 *confirmed,
    QString *errorName, QString *errorMessage) {
    const bool ok = deleteSavedLayoutInternal(expectedSnapshotRevision, layoutId, true, nullptr, errorName, errorMessage);
    if (confirmed) *confirmed = savedLayoutsSnapshot();
    return ok;
}

bool DeviceManager::deleteSavedLayoutV2(quint64 expectedSnapshotRevision, const QString &layoutId,
    TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage) {
    return deleteSavedLayoutInternal(expectedSnapshotRevision, layoutId, false, confirmed, errorName, errorMessage);
}

bool DeviceManager::deleteSavedLayoutInternal(quint64 expectedSnapshotRevision, const QString &layoutId,
    bool legacyInterface, TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto fail =
        [this, confirmed, errorName, errorMessage](
            const QString &name, const QString &message) {
            if (confirmed) {
                *confirmed = savedLayoutsSnapshotV2();
            }
            if (errorName) {
                *errorName = name;
            }
            if (errorMessage) {
                *errorMessage = message;
            }
            return false;
        };
    if (runtimeDowngradeV10Prepared_) {
        return fail(
            QStringLiteral(
                "org.tryx.Panorama.Error.DowngradePrepared"),
            tr("Saved layouts are locked after runtime downgrade preparation"));
    }
    const TryxRuntimeSavedLayoutsSnapshotV2 current = savedLayoutsSnapshotV2();
    if (current.status != QStringLiteral("Ready")) {
        return fail(
            current.status == QStringLiteral("Unsupported")
                ? QStringLiteral(
                      "org.tryx.Panorama.Error.UnsupportedProduct")
                : QStringLiteral(
                      "org.tryx.Panorama.Error.SavedLayoutsUnavailable"),
            current.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : current.diagnostic);
    }
    const tryx::SavedLayoutStore::MutationResult result = legacyInterface
        ? savedLayoutStore_->remove(expectedSnapshotRevision, current.deviceIdentity, current.productId, layoutId)
        : savedLayoutStore_->removeV2(expectedSnapshotRevision, current.deviceIdentity, current.productId, layoutId);
    if (!result.ok()) {
        if (result.commitMayExist ||
            result.code ==
                tryx::SavedLayoutStore::ErrorCode::CommitUnknown) {
            savedLayoutsFailureDetail_ = result.detail.left(512);
        }
        return fail(
            savedLayoutStoreErrorName(result.code),
            result.detail.isEmpty()
                ? tr("The saved layout could not be deleted")
                : result.detail);
    }
    if (confirmed) {
        *confirmed = savedLayoutsSnapshotV2();
    }
    return true;
}

bool DeviceManager::acquireFirmwareExclusive(
    const QString &leaseId, QString *errorMessage) {
    return sessionController_.acquireFirmwareExclusive(leaseId, errorMessage);
}

void DeviceManager::releaseFirmwareExclusive(
    const QString &leaseId, bool resumeTransport) {
    sessionController_.releaseFirmwareExclusive(leaseId, resumeTransport);
}

void DeviceManager::
    setFirmwareRecoveryInterlockActive(
        bool active) {
    sessionController_.setFirmwareRecoveryInterlockActive(active);
}

void DeviceManager::
    resumeConnectionAfterFirmwareRecoveryAcknowledgement() {
    sessionController_.resumeConnectionAfterFirmwareRecoveryAcknowledgement();
}

DeviceManager::DeviceManager(PrinterDeviceMonitor *printerMonitor,
                             bool startPrinterMonitor, QObject *parent)
    : QObject(parent), sessionController_(sessionCallbacks()),
      worker_(new DeviceWorker),
      printerMediaPreparer_(new PrinterMediaPreparer),
      printerMonitor_(printerMonitor),
      runtimePresentationPreferencesStore_(
          std::make_unique<tryx::RuntimePresentationPreferencesStore>()),
      savedLayoutStore_(std::make_unique<tryx::SavedLayoutStore>()),
      runtimeDowngradeStore_(std::make_unique<tryx::RuntimeDowngradeStore>()) {
    automaticPrinterSessionStart_ = startPrinterMonitor;
    printerMonitor_->setParent(this);
    qRegisterMetaType<PrinterProtocol::UsbPrinterDevice>();
    qRegisterMetaType<PrinterProtocol::DiscoverySnapshot>();
    qRegisterMetaType<PrinterProtocol::DeviceInfo>();
    qRegisterMetaType<PrinterProtocol::DeviceSpecifications>();
    qRegisterMetaType<PrinterProtocol::MediaFile>();
    qRegisterMetaType<QList<PrinterProtocol::MediaFile>>();
    qRegisterMetaType<PrinterProtocol::MutationOutcome>();
    qRegisterMetaType<PrinterProtocol::PaseOverlayConfig>();
    qRegisterMetaType<PrinterProtocol::PaseDisplayState>();
    qRegisterMetaType<PrinterProtocol::PaseDisplayStateResult>();
    qRegisterMetaType<TryxRuntimeOperationInfo>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayState>();
    qRegisterMetaType<TryxRuntimeDeviceMediaArtifact>();
    qRegisterMetaType<TryxRuntimePresentationPreferencesV1>();
    qRegisterMetaType<QList<TryxRuntimeSavedMediaRefV1>>();
    qRegisterMetaType<RecoveredH264ProbeMetadata>();

    connect(&sessionController_,
            &PrinterSessionController::printerDisplaySessionChanged, this,
            &DeviceManager::printerDisplaySessionChanged, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestCancelPrinterPreparation, this,
            &DeviceManager::requestCancelPrinterPreparation,
            Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerOperationsCancelled, this,
            &DeviceManager::printerOperationsCancelled, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::mediaListUpdated,
            this, &DeviceManager::mediaListUpdated, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::deviceDisconnected,
            this, &DeviceManager::deviceDisconnected, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerPresenceChanged, this,
            &DeviceManager::printerPresenceChanged, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerDeviceVersionsReady, this,
            &DeviceManager::printerDeviceVersionsReady, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestConfigurePrinter, this,
            &DeviceManager::requestConfigurePrinter, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestRestorePrinterOverlay, this,
            &DeviceManager::requestRestorePrinterOverlay, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::uploadStatus, this,
            &DeviceManager::uploadStatus, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestStartPrinterSession, this,
            &DeviceManager::requestStartPrinterSession, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::requestClearPrinter,
            this, &DeviceManager::requestClearPrinter, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::requestDisconnect,
            this, &DeviceManager::requestDisconnect, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::deviceError, this,
            &DeviceManager::deviceError, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::requestConnect,
            this, &DeviceManager::requestConnect, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerDeviceInfoFailed, this,
            &DeviceManager::printerDeviceInfoFailed, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestPrinterDeviceInfo, this,
            &DeviceManager::requestPrinterDeviceInfo, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::deviceConnected,
            this, &DeviceManager::deviceConnected, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestFirmwareTransportQuiesce, this,
            &DeviceManager::requestFirmwareTransportQuiesce,
            Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestFirmwareQuiesceReleaseFence, this,
            &DeviceManager::requestFirmwareQuiesceReleaseFence,
            Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::metricsStateUpdated,
            this, &DeviceManager::metricsStateUpdated, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::displayStateUpdated,
            this, &DeviceManager::displayStateUpdated, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::displaySnapshotChangedV1,
            this, &DeviceManager::displaySnapshotChangedV1, Qt::DirectConnection);
    connect(&operationCoordinator_, &PrinterOperationCoordinator::displayMutationStarted,
            &sessionController_, &PrinterSessionController::beginDisplayMutation, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::brightnessChanged,
            this, &DeviceManager::brightnessChanged, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::screenConfigChanged,
            this, &DeviceManager::screenConfigChanged, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::mediaUploaded, this,
            &DeviceManager::mediaUploaded, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::mediaDeleted, this,
            &DeviceManager::mediaDeleted, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::requestPrinterDisplayState, this,
            &DeviceManager::requestPrinterDisplayState, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::firmwareTransportQuiesced, this,
            &DeviceManager::firmwareTransportQuiesced, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerDeviceInfoReady, this,
            &DeviceManager::printerDeviceInfoReady, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::sysinfoSent, this,
            &DeviceManager::sysinfoSent, Qt::DirectConnection);
    connect(&sessionController_,
            &PrinterSessionController::printerTransportReady, this,
            &DeviceManager::printerTransportReady, Qt::DirectConnection);
    connect(&sessionController_, &PrinterSessionController::requestKeepalive,
            this, &DeviceManager::requestKeepalive, Qt::DirectConnection);
    connect(
        &sessionController_, &PrinterSessionController::requestGenerationGate,
        this,
        [this](quint64 generation, bool open) {
            if (worker_)
                worker_->updatePrinterGenerationGate(generation, open);
        },
        Qt::DirectConnection);

    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationChanged,
        this, &DeviceManager::operationChanged,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationRemoved,
        this, &DeviceManager::operationRemoved,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaCatalogUpdated,
        this, &DeviceManager::mediaCatalogUpdated,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaListUpdated,
        this, &DeviceManager::mediaListUpdated,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::mediaUploaded,
        this, &DeviceManager::mediaUploaded,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationError,
        this, &DeviceManager::deviceError,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestEndForegroundOperation,
        this, &DeviceManager::requestEndPrinterForegroundOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestClearWorkerCancellation,
        this,
        [this](const QString &operationId) {
            if (worker_) {
                worker_->clearPrinterOperationCancellation(operationId);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestApplyDeferredMediaCatalog, this,
        [this](const QList<PrinterProtocol::MediaFile> &mediaFiles,
               quint64 generation, const QString &deviceIdentity) {
            if (sessionController_.state().printerGeneration == generation &&
                sessionController_.state().printerDeviceSerial.trimmed() ==
                    deviceIdentity) {
                updateMediaCatalog(mediaFiles);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestArtifactSweep,
        this, &DeviceManager::sweepDeviceMediaArtifacts,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestUnwatchArtifactOwner,
        this,
        [this](const QString &ownerUniqueName) {
            if (artifactOwnerWatcher_) {
                artifactOwnerWatcher_->removeWatchedService(
                    ownerUniqueName);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestWatchArtifactOwner,
        this,
        [this](const QString &ownerUniqueName, bool *registered) {
            if (registered) {
                *registered = watchArtifactOwner(ownerUniqueName);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestBeginForegroundOperation,
        this, &DeviceManager::requestBeginPrinterForegroundOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestStageMedia,
        this, &DeviceManager::requestPrinterStageMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelOperation,
        this, &DeviceManager::cancelOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestReplacePreflight,
        this, &DeviceManager::requestPrinterReplacePreflight,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRecoveredMedia,
        this, &DeviceManager::requestPrepareRecoveredPrinterMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRecoveredMediaWithProfile,
        this,
        &DeviceManager::
            requestPrepareRecoveredPrinterMediaWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestAnalyzeSource,
        this, &DeviceManager::requestAnalyzePrinterSource,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestAnalyzeSourceWithProfile,
        this,
        &DeviceManager::requestAnalyzePrinterSourceWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareMedia,
        this, &DeviceManager::requestPreparePrinterMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareMediaWithProfile,
        this,
        &DeviceManager::requestPreparePrinterMediaWithPreparationProfile,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestDeleteMedia,
        this, &DeviceManager::requestPrinterDeleteMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestApplyMedia,
        this, &DeviceManager::requestPrinterApplyMedia,
        Qt::DirectConnection);
    connect(&operationCoordinator_, &PrinterOperationCoordinator::requestApplyMediaWithBadgesV1,
            this, &DeviceManager::requestPrinterApplyMediaWithBadgesV1, Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestConfigureMetrics,
        this, &DeviceManager::requestPrinterConfigureMetrics,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestRefreshMedia,
        this, &DeviceManager::requestPrinterRefreshMedia,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestDispatchPreparedUpload,
        this,
        [this](const QString &devicePath, const QString &operationId,
               quint64 generation) {
            operationCoordinator_.dispatchPreparedUploadWithRetryBarrier(
                operationContext(), devicePath, operationId, generation);
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestUploadPrepared,
        this, &DeviceManager::requestPrinterUploadPrepared,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrinterRecovery,
        this, &DeviceManager::requirePrinterRecovery,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestStartRetryCacheReadOnlyReconciliation,
        this,
        [this]() {
            startRetryCacheReadOnlyReconciliationIfReady();
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestResumePrinterSessionAfterRetryCacheValidation,
        this,
        &DeviceManager::resumePrinterSessionAfterRetryCacheValidation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestValidateRetryCacheArtifact,
        this, &DeviceManager::requestValidatePrinterRetryCacheArtifact,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelRetryCacheValidation,
        this,
        [this](const QString &validationToken) {
            if (printerMediaPreparer_) {
                printerMediaPreparer_->cancelRetryValidation(
                    validationToken);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestClearRetryCacheValidationCancellation,
        this,
        [this](const QString &validationToken) {
            if (printerMediaPreparer_) {
                printerMediaPreparer_->clearRetryValidationCancellation(
                    validationToken);
            }
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestPrepareRestrictedReadOnlySession,
        this,
        [this](quint64 generation) {
            sessionController_.prepareRestrictedReadOnlySession(generation);
        },
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::operationsCancelled,
        this, &DeviceManager::printerOperationsCancelled,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::
            requestPromoteRestrictedSessionAfterProof,
        this, &DeviceManager::promoteRestrictedSessionAfterProof,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelPreparationOperation,
        this, &DeviceManager::requestCancelPrinterPreparationOperation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestReleasePreparationPath,
        this, &DeviceManager::requestReleasePrinterPreparation,
        Qt::DirectConnection);
    connect(
        &operationCoordinator_,
        &PrinterOperationCoordinator::requestCancelWorkerOperation,
        this,
        [this](const QString &operationId) {
            if (worker_) {
                worker_->cancelPrinterOperation(operationId);
            }
        },
        Qt::DirectConnection);

    artifactOwnerWatcher_ = new QDBusServiceWatcher(this);
    artifactOwnerWatcher_->setConnection(
        QDBusConnection::sessionBus());
    artifactOwnerWatcher_->setWatchMode(
        QDBusServiceWatcher::WatchForUnregistration);
    connect(
        artifactOwnerWatcher_,
        &QDBusServiceWatcher::serviceUnregistered,
        this, &DeviceManager::handleArtifactOwnerUnregistered);
    artifactSweepTimer_ = new QTimer(this);
    artifactSweepTimer_->setInterval(kDeviceMediaSweepIntervalMs);
    connect(
        artifactSweepTimer_, &QTimer::timeout,
        this, &DeviceManager::sweepDeviceMediaArtifacts);

    PrinterOverlayLeaseMode selectedOverlayLeaseMode =
        PrinterOverlayLeaseMode::PingAndOverlayLease;
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
            selectedOverlayLeaseMode =
                PrinterOverlayLeaseMode::PingOnly;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        } else {
            selectedOverlayLeaseMode =
                PrinterOverlayLeaseMode::PingAndOverlayLease;
            overlayLeaseConfigStatus =
                configFileExists && !configPathError
                    ? QStringLiteral("loaded")
                    : QStringLiteral("default-no-config");
        }
    }
    setPrinterOverlayLeaseMode(selectedOverlayLeaseMode);
    if (startPrinterMonitor) {
        loadRuntimePresentationPreferences();
    }
    if (startPrinterMonitor) {
        logPrinterLifecycleEvent(
            QStringLiteral("overlay_lease_mode_selected"),
            sessionController_.state().printerGeneration,
            {{QStringLiteral("lease_mode"),
              printerOverlayLeaseModeName(
                  sessionController_.state().printerOverlayLeaseMode)},
             {QStringLiteral("config_status"), overlayLeaseConfigStatus}});
    }

    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    printerMediaPreparer_->moveToThread(&printerPreparationThread_);
    connect(&printerPreparationThread_, &QThread::finished,
            printerMediaPreparer_, &QObject::deleteLater);

    connect(worker_, &DeviceWorker::connected, this,
            [this](const QString &pid, const QString &serial, const QString &fw,
                   const QString &app) {
                sessionController_.handleWorkerConnected(pid, serial, fw, app);
            });
    connect(worker_, &DeviceWorker::disconnected, this,
            [this]() { sessionController_.handleWorkerDisconnected(); });

    connect(worker_, &DeviceWorker::error, this,
            [this](const QString &message) {
                sessionController_.handleWorkerError(message);
            });
    connect(worker_, &DeviceWorker::brightnessSet, this, [this](int value) {
        sessionController_.handleWorkerBrightnessSet(value);
    });
    connect(worker_, &DeviceWorker::screenConfigSet, this,
            [this]() { sessionController_.handleWorkerScreenConfigSet(); });
    connect(worker_, &DeviceWorker::mediaUploaded, this,
            [this](const QString &fileName) {
                sessionController_.handleWorkerMediaUploaded(fileName);
            });
    connect(worker_, &DeviceWorker::mediaDeleted, this,
            [this]() { sessionController_.handleWorkerMediaDeleted(); });
    connect(worker_, &DeviceWorker::mediaListReady, this,
            [this](const QStringList &files) {
                sessionController_.handleWorkerMediaListReady(files);
            });
    connect(worker_, &DeviceWorker::uploadProgress, this,
            [this](const QString &status) {
                sessionController_.handleWorkerUploadProgress(status);
            });

    connect(worker_, &DeviceWorker::printerOperationError, this,
            [this](const QString &message, quint64 generation) {
                sessionController_.handleWorkerPrinterOperationError(
                    message, generation);
            });
    connect(worker_, &DeviceWorker::printerUploadProgress, this,
            [this](const QString &status, quint64 generation) {
                sessionController_.handleWorkerPrinterUploadProgress(
                    status, generation);
            });
    connect(worker_, &DeviceWorker::printerSessionStarted, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterSessionStarted(
                    generation);
            });
    connect(worker_, &DeviceWorker::printerSessionStopped, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterSessionStopped(
                    generation);
            });
    connect(worker_, &DeviceWorker::firmwareTransportQuiesced, this,
            [this](const QString &leaseId, quint64 generation) {
                sessionController_.handleWorkerFirmwareTransportQuiesced(
                    leaseId, generation);
            });
    connect(
        worker_, &DeviceWorker::firmwareQuiesceReleaseFenceReached, this,
        [this](const QString &leaseId, quint64 generation) {
            sessionController_.handleWorkerFirmwareQuiesceReleaseFenceReached(
                leaseId, generation);
        });
    connect(worker_, &DeviceWorker::printerSessionLost, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterSessionLost(generation);
            });
    connect(worker_, &DeviceWorker::printerForegroundProgress, this,
            [this](const QString &operationId, const QString &stage,
                   qint64 completed, qint64 total, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handleForegroundProgress(
                    operationContext(), operationId, stage, completed,
                    total, message, generation);
            });
    connect(worker_, &DeviceWorker::printerMediaStaged, this,
            [this](const QString &operationId, const QString &mediaName,
                   const QString &outputPath, bool success, bool cancelled,
                   qint64 fileSize, qint64 chunkCount,
                   const QString &rawSha256,
                   const QString &decodedSha256,
                   const RecoveredH264ProbeMetadata &probeMetadata,
                   const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleMediaStaged(
                    operationContext(), operationId, mediaName, outputPath,
                    success, cancelled, fileSize, chunkCount, rawSha256,
                    decodedSha256, probeMetadata, errorMessage, generation);
            });
    connect(
        worker_, &DeviceWorker::printerReplacePreflightFinished,
        this,
        [this](
            const QString &operationId, const QString &mediaName,
            const QString &expectedReplacementName,
            qint64 expectedReplacementSize,
            const QStringList &references,
            const QStringList &referencingSlots,
            const QString &activeScreenMode,
            const QString &activePlayMode,
            const QStringList &activeMedia,
            bool originalIdentityVerified,
            bool replacementIdentityVerified, bool success,
            const QString &errorMessage, quint64 generation) {
            operationCoordinator_.handleReplacePreflightFinished(
                operationContext(), operationId, mediaName,
                expectedReplacementName, expectedReplacementSize,
                references, referencingSlots, activeScreenMode,
                activePlayMode, activeMedia, originalIdentityVerified,
                replacementIdentityVerified, success, errorMessage,
                generation);
        });

    connect(worker_, &DeviceWorker::printerUploadFinished, this,
            [this](
                const QString &operationId, const QString &uploadPath,
                const QString &remoteName, bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleUploadFinished(
                    operationContext(), operationId, uploadPath,
                    remoteName, success, outcome, errorMessage,
                    generation);
            });
    connect(worker_, &DeviceWorker::printerMediaListReady, this,
            [this](
                const QString &operationId,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                quint64 generation) {
                operationCoordinator_.handleMediaListReady(
                    operationContext(), operationId, mediaFiles,
                    generation);
            });

    connect(worker_, &DeviceWorker::printerMediaListFailed, this,
            [this](
                const QString &operationId, const QString &message,
                quint64 generation) {
                operationCoordinator_.handleMediaListFailed(
                    operationContext(), operationId, message,
                    generation);
            });
    connect(worker_, &DeviceWorker::printerDeleteFinished, this,
            [this](
                const QString &operationId,
                const QStringList &requestedNames,
                const QStringList &deletedNames,
                const QList<PrinterProtocol::MediaFile> &mediaFiles,
                bool success,
                PrinterProtocol::MutationOutcome outcome,
                const QString &errorMessage, quint64 generation) {
                operationCoordinator_.handleDeleteFinished(
                    operationContext(), operationId, requestedNames,
                    deletedNames, mediaFiles, success, outcome,
                    errorMessage, generation);
            });
    connect(worker_, &DeviceWorker::printerSavedLayoutProofFailed, this,
            [this](
                const QString &operationId,
                const QString &errorCategory,
                const QString &errorMessage,
                quint64 generation) {
                operationCoordinator_.handleSavedLayoutProofFailed(
                    operationContext(), operationId, errorCategory,
                    errorMessage, generation);
                sessionController_.finishDisplayMutation(operationId, generation, true);
            });
    connect(worker_, &DeviceWorker::printerApplyFinished, this,
            [this](const QString &operationId, const QString &mediaFile,
                   bool success, bool metricsUpdated,
                   PrinterProtocol::MutationOutcome outcome,
                   const QString &errorMessage, quint64 generation,
                   const PrinterProtocol::PaseDisplayStateResult &readback) {
                const auto completedContext = operationContext();
                operationCoordinator_.handleApplyFinished(
                    completedContext, operationId, mediaFile, success,
                    metricsUpdated, outcome, errorMessage, generation,
                    [this](const QString &deviceIdentity) {
                        return persistedPaseOverlayForDevice(
                            deviceIdentity);
                    },
                    [this, &operationId, generation](const PrinterProtocol::PaseOverlayConfig &overlay,
                           bool enabled, QString *persistenceError) {
                        if (!sessionController_.displayMutationCanPersistOverlay(operationId, generation)) {
                            if (persistenceError) *persistenceError = tr("The previous overlay is not confirmed; apply a complete layout to resolve it");
                            return false;
                        }
                        return persistPaseMetricsConfiguration(
                            overlay, enabled, persistenceError);
                    },
                    [this](const PrinterProtocol::PaseOverlayConfig &overlay,
                           bool enabled, const QString &diagnostic,
                           bool updateDisplay) {
                        sessionController_.publishOperationMetrics(
                            overlay, enabled, diagnostic, updateDisplay);
                    },
                    [this]() { emit screenConfigChanged(); },
                    [this, &operationId, generation, &completedContext, &readback, outcome](const PrinterProtocol::PaseOverlayConfig &overlay) {
                        if (outcome == PrinterProtocol::MutationOutcome::Succeeded)
                            sessionController_.commitDisplayMutation(operationId, generation,
                                completedContext.deviceIdentity, completedContext.productId, readback, overlay);
                    });
                const bool noMutation = !success && (outcome == PrinterProtocol::MutationOutcome::NotStarted
                    || outcome == PrinterProtocol::MutationOutcome::Rejected || outcome == PrinterProtocol::MutationOutcome::Cancelled);
                sessionController_.finishDisplayMutation(operationId, generation, noMutation);
            });
    connect(worker_, &DeviceWorker::printerMetricsConfigured, this,
            [this](const QString &operationId, bool success,
                   PrinterProtocol::MutationOutcome outcome,
                   const QString &errorMessage, quint64 generation) {
                const auto completedContext = operationContext();
                operationCoordinator_.handleMetricsConfigured(
                    completedContext, operationId, success, outcome,
                    errorMessage, generation,
                    [this](const PrinterProtocol::PaseOverlayConfig &overlay,
                           bool enabled, QString *persistenceError) {
                        return persistPaseMetricsConfiguration(
                            overlay, enabled, persistenceError);
                    },
                    [this](const PrinterProtocol::PaseOverlayConfig &overlay,
                           bool enabled, const QString &diagnostic,
                           bool updateDisplay) {
                        sessionController_.publishOperationMetrics(
                            overlay, enabled, diagnostic, updateDisplay);
                    },
                    [this, &operationId, generation, &completedContext, outcome](const PrinterProtocol::PaseOverlayConfig &overlay) {
                        if (outcome == PrinterProtocol::MutationOutcome::Succeeded)
                            sessionController_.commitMetricsDisplayMutation(operationId, generation,
                                completedContext.deviceIdentity, completedContext.productId, overlay);
                    });
                const bool noMutation = !success && (outcome == PrinterProtocol::MutationOutcome::NotStarted
                    || outcome == PrinterProtocol::MutationOutcome::Rejected || outcome == PrinterProtocol::MutationOutcome::Cancelled);
                sessionController_.finishDisplayMutation(operationId, generation, noMutation);
            });
    connect(
        worker_, &DeviceWorker::printerMetricsAvailabilityChanged, this,
        [this](const QStringList &availableMetrics, quint64 generation) {
            sessionController_.handleWorkerPrinterMetricsAvailabilityChanged(
                availableMetrics, generation);
        });
    connect(worker_, &DeviceWorker::printerScreenConfigSet, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterScreenConfigSet(
                    generation);
            });
    connect(worker_, &DeviceWorker::printerDeviceVersionsReady, this,
            [this](const QString &firmware, const QString &appVersion,
                   quint64 generation) {
                sessionController_.handleWorkerPrinterDeviceVersionsReady(
                    firmware, appVersion, generation);
            });
    connect(worker_, &DeviceWorker::printerDeviceSpecificationsReady, this,
            [this](const PrinterProtocol::DeviceSpecifications &specifications,
                   const QString &devicePath, const QString &deviceSerial,
                   quint16 productId, quint64 generation) {
                sessionController_.handleWorkerPrinterDeviceSpecificationsReady(
                    specifications, devicePath, deviceSerial, productId,
                    generation);
            });
    connect(
        worker_, &DeviceWorker::printerDeviceInfoReady, this,
        [this](const PrinterProtocol::DeviceInfo &info, quint64 generation) {
            sessionController_.handleWorkerPrinterDeviceInfoReady(info,
                                                                  generation);
        });
    connect(worker_, &DeviceWorker::printerDeviceInfoFailed, this,
            [this](const QString &message, quint64 generation) {
                sessionController_.handleWorkerPrinterDeviceInfoFailed(
                    message, generation);
            });
    connect(worker_, &DeviceWorker::printerDisplayStateReady, this,
            [this](const PrinterProtocol::PaseDisplayState &state,
                   quint64 generation) {
                sessionController_.handleWorkerPrinterDisplayStateReady(
                    state, generation);
            });
    connect(worker_, &DeviceWorker::printerDisplayStateFailed, this,
            [this](const QString &message, quint64 generation) {
                sessionController_.handleWorkerPrinterDisplayStateFailed(
                    message, generation);
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
    connect(this, &DeviceManager::requestFirmwareTransportQuiesce,
            worker_, &DeviceWorker::quiesceForFirmware);
    connect(
        this,
        &DeviceManager::requestFirmwareQuiesceReleaseFence,
        worker_,
        &DeviceWorker::releaseFirmwareQuiesceFence);
    connect(this, &DeviceManager::requestPrinterDeviceInfo,
            worker_, &DeviceWorker::readPrinterDeviceInfo);
    connect(this, &DeviceManager::requestPrinterDisplayState,
            worker_, &DeviceWorker::readPrinterDisplayState);
    connect(this, &DeviceManager::requestAnalyzePrinterSource,
            printerMediaPreparer_, &PrinterMediaPreparer::analyzeSource);
    connect(
        this,
        &DeviceManager::requestAnalyzePrinterSourceWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::analyzeSourceWithPreparationProfile);
    connect(this, &DeviceManager::requestPrinterUploadPrepared,
            worker_, &DeviceWorker::uploadPreparedPrinterMedia);
    connect(this, &DeviceManager::requestPrinterRefreshMedia,
            worker_, &DeviceWorker::refreshPrinterMediaList);
    connect(this, &DeviceManager::requestPrinterStageMedia,
            worker_, &DeviceWorker::stagePrinterMedia);
    connect(this, &DeviceManager::requestPrinterReplacePreflight,
            worker_, &DeviceWorker::preflightReplacePrinterMedia);
    connect(this, &DeviceManager::requestPrinterDeleteMedia,
            worker_, &DeviceWorker::deletePrinterMedia);
    connect(this, &DeviceManager::requestPrinterApplyMedia,
            worker_, &DeviceWorker::applyPrinterMedia);
    connect(this, &DeviceManager::requestPrinterApplyMediaWithBadgesV1,
            worker_, &DeviceWorker::applyPrinterMediaWithBadgesV1);
    connect(this, &DeviceManager::requestPrinterConfigureMetrics,
            worker_, &DeviceWorker::configurePrinterMetrics);
    connect(this, &DeviceManager::requestPrinterSysinfo,
            worker_, &DeviceWorker::sendPrinterSysinfo);
    if (automaticPrinterSessionStart_) {
        connect(this, &DeviceManager::requestStartPrinterSession,
                worker_, &DeviceWorker::startPrinterDisplaySession);
    }
    connect(worker_, &DeviceWorker::sysinfoSent, this,
            [this]() { sessionController_.handleWorkerSysinfoSent(); });
    connect(worker_, &DeviceWorker::printerSysinfoSent, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterSysinfoSent(generation);
            });
    connect(worker_, &DeviceWorker::printerSysinfoFailed, this,
            [this](const QString &message, quint64 generation) {
                sessionController_.handleWorkerPrinterSysinfoFailed(message,
                                                                    generation);
            });
    connect(worker_, &DeviceWorker::printerTransportReady, this,
            [this](quint64 generation) {
                sessionController_.handleWorkerPrinterTransportReady(
                    generation);
            });

    connect(this, &DeviceManager::requestPreparePrinterMedia,
            printerMediaPreparer_, &PrinterMediaPreparer::prepare);
    connect(
        this,
        &DeviceManager::requestPreparePrinterMediaWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::prepareWithPreparationProfile);
    connect(this, &DeviceManager::requestPrepareRecoveredPrinterMedia,
            printerMediaPreparer_,
            &PrinterMediaPreparer::prepareRecovered);
    connect(
        this,
        &DeviceManager::requestPrepareRecoveredPrinterMediaWithPreparationProfile,
        printerMediaPreparer_,
        &PrinterMediaPreparer::prepareRecoveredWithPreparationProfile);
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
    connect(
        this,
        &DeviceManager::requestValidatePrinterRetryCacheArtifact,
        printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCacheArtifact);
    connect(
        printerMediaPreparer_,
        &PrinterMediaPreparer::retryCacheArtifactValidated,
        this,
        &DeviceManager::handleRetryCacheArtifactValidation);
    connect(worker_, &DeviceWorker::printerPreparedFileConsumed, this,
            [this](const QString &uploadPath) {
                releasePrinterPreparationPath(uploadPath);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::sourceAnalyzed,
            this,
            [this](
                const QString &operationId, const QString &localPath,
                const QString &contentSha256, qint64 sourceSize,
                const QString &conversionProfile, quint64 generation) {
                operationCoordinator_.handleSourceAnalyzed(
                    operationContext(), operationId, localPath,
                    contentSha256, sourceSize, conversionProfile,
                    generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::progress, this,
            [this](const QString &operationId, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handlePreparationProgress(
                    operationContext(), operationId, message, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::failed, this,
            [this](const QString &operationId, const QString &message,
                   quint64 generation) {
                operationCoordinator_.handlePreparationFailed(
                    operationContext(), operationId, message, generation);
            });
    connect(printerMediaPreparer_, &PrinterMediaPreparer::prepared, this,
            [this](
                const QString &operationId, const QString &devicePath,
                const QString &sourcePath, const QString &uploadPath,
                const QString &remoteName, const QString &preparedSha256,
                const QString &stagedThumbnailPath,
                const QString &stagedThumbnailSha256, quint64 generation) {
                operationCoordinator_.handlePrepared(
                    operationContext(), operationId, devicePath,
                    sourcePath, uploadPath, remoteName, preparedSha256,
                    stagedThumbnailPath, stagedThumbnailSha256,
                    generation);
            });


    connect(printerMonitor_, &PrinterDeviceMonitor::snapshotChanged,
            this, &DeviceManager::handlePrinterSnapshot);
    connect(printerMonitor_, &PrinterDeviceMonitor::currentEndpointRemoved,
            this,
            [this]() { sessionController_.handleCurrentEndpointRemoved(); });
    connect(printerMonitor_, &PrinterDeviceMonitor::monitorError,
            this, &DeviceManager::deviceError);

    printerPreparationThread_.start();
    workerThread_.start();
    if (startPrinterMonitor) {
        // The metadata scan is bounded and does not hash media. Reserve the
        // scheduler before the D-Bus service can accept foreground work; the
        // potentially large SHA-256 validation remains queued on the
        // preparation thread.
        cleanupMediaRuntimeStaging();
        cleanupDeviceMediaOutbox();
        loadMediaCatalogStore();
        loadSavedLayoutsStore();
        loadPaseMetricsConfig();
        loadRetryCache();
        loadReplaceJournal();
        loadDeleteIntent();
        artifactSweepTimer_->start();
        printerMonitor_->start();
    }
}

#ifdef TRYX_PROTOCOL_TESTING
DeviceManager *DeviceManager::createForTesting(
    const QString &sysfsRoot, const QString &devRoot, QObject *parent) {
    auto *monitor = new PrinterDeviceMonitor;
    monitor->setDiscoveryRootsForTesting(sysfsRoot, devRoot);
    auto *manager = new DeviceManager(monitor, false, parent);
    manager->operationCoordinator_.retryCacheDirectoryOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("retry-cache"));
    manager->operationCoordinator_.mediaCatalogStore_ =
        std::make_unique<tryx::MediaCatalogStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("media-catalog")));
    manager->sessionController_.paseMetricsConfigStore_ =
        std::make_unique<tryx::PaseMetricsConfigStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("pase-config")));
    manager->runtimePresentationPreferencesStore_ =
        std::make_unique<tryx::RuntimePresentationPreferencesStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral(
                    "runtime-presentation-preferences")));
    manager->savedLayoutStore_ =
        std::make_unique<tryx::SavedLayoutStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral("saved-layouts")));
    manager->runtimeDowngradeStore_ =
        std::make_unique<tryx::RuntimeDowngradeStore>(
            QDir(QFileInfo(sysfsRoot).absolutePath())
                .filePath(QStringLiteral(
                    "runtime-downgrade")));
    manager->loadRuntimePresentationPreferences();
    manager->loadSavedLayoutsStore();
    manager->operationCoordinator_.mediaRuntimeRootOverride_ =
        QDir(QFileInfo(sysfsRoot).absolutePath())
            .filePath(QStringLiteral("runtime-staging"));
    manager->operationCoordinator_.deviceMediaArtifactStore_ =
        std::make_unique<tryx::DeviceMediaArtifactStore>(
            QDir(manager->operationCoordinator_.mediaRuntimeRootOverride_)
                .filePath(QStringLiteral("device-media-outbox")));
    manager->cleanupDeviceMediaOutbox();
    manager->artifactSweepTimer_->start();
    manager->loadMediaCatalogStore();
    manager->loadRetryCache();
    return manager;
}

void DeviceManager::setAutoConnectModeForTesting(bool enabled) {
    sessionController_.setAutoConnectModeForTesting(enabled);
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
    return sessionController_.state().printerGeneration;
}

bool DeviceManager::printerDisplaySessionActiveForTesting() const {
    return sessionController_.state().printerDisplaySessionActive;
}

bool DeviceManager::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    const bool adopted = QMetaObject::invokeMethod(
        worker_,
        [this, fd, devicePath]() {
            worker_->adoptPrinterFileDescriptorForTesting(fd, devicePath);
        },
        Qt::BlockingQueuedConnection);
    if (adopted && sessionController_.state().printerClassConnected &&
        devicePath == currentPrinterPath() && !retryCacheMutationGateActive()) {
        emit requestStartPrinterSession(
            devicePath, sessionController_.state().printerGeneration);
        if (!automaticPrinterSessionStart_) {
            QMetaObject::invokeMethod(
                worker_,
                [this, devicePath,
                 generation = sessionController_.state().printerGeneration]() {
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

DeviceManager::~DeviceManager() {
    if (artifactSweepTimer_) {
        artifactSweepTimer_->stop();
    }
    operationCoordinator_.cancelPendingRetryCacheValidations();
    disconnect(printerMonitor_, nullptr, this, nullptr);
    sessionController_.shutdownBeforeWorkersStopped();
    if (printerPreparationThread_.isRunning()) {
        QMetaObject::invokeMethod(printerMediaPreparer_, "shutdown",
                                  Qt::BlockingQueuedConnection);
        printerPreparationThread_.quit();
        printerPreparationThread_.wait();
    }
    workerThread_.quit();
    workerThread_.wait();
    operationCoordinator_.shutdownAfterWorkersStopped();
}

void DeviceManager::setPrinterDisplaySessionActive(bool active) {
    sessionController_.setPrinterDisplaySessionActive(active);
}

void DeviceManager::clearDeviceSpecificationsCache() {
    sessionController_.clearDeviceSpecificationsCache();
}

PrinterOperationContext DeviceManager::operationContext() const {
    PrinterOperationContext context;
    context.devicePath = currentPrinterPath();
    context.deviceIdentity =
        sessionController_.state().printerDeviceSerial.trimmed();
    context.unavailableStatusText = printerUnavailableStatusText();
    context.mutationUnavailableStatusText =
        printerMutationUnavailableStatusText();
    context.firmwareExclusiveStatusText =
        firmwareExclusiveStatusText();
    context.productId = sessionController_.state().printerProductId;
    context.generation = sessionController_.state().printerGeneration;
    context.connected = sessionController_.state().connected;
    context.printerClassConnected =
        sessionController_.state().printerClassConnected;
    context.printerEndpointReady =
        sessionController_.state().printerSnapshot.state ==
        PrinterProtocol::DiscoveryState::Ready;
    context.displaySessionActive =
        sessionController_.state().printerDisplaySessionActive;
    context.displaySessionLost =
        sessionController_.state().printerDisplaySessionLost;
    context.recoveryRequired =
        sessionController_.state().printerRecoveryRequired;
    context.runtimeDowngradePrepared = runtimeDowngradeV10Prepared_;
    context.firmwareExclusiveActive = firmwareExclusiveActive();
    context.firmwareReleasePending =
        !sessionController_.state().firmwareReleasePendingLeaseId.isEmpty();
    context.firmwareRecoveryInterlockActive =
        sessionController_.state().firmwareRecoveryInterlockActive;
    const auto profile = currentPrinterProductProfile();
    if (profile) {
        context.supportsMediaCatalog =
            profile->mediaCatalogSupported;
        context.supportsDisplayConfiguration =
            profile->displayConfigurationSupported;
        context.supportsOverlayMetrics =
            profile->overlayMetricsSupported;
        context.supportsSplitAreaMedia =
            profile->splitAreaMediaSupported;
    }
    context.displayState = sessionController_.state().displayState;
    context.displayStateGeneration =
        sessionController_.state().displayStateReadGeneration;
    return context;
}

void DeviceManager::handlePrinterSnapshot(
    const PrinterProtocol::DiscoverySnapshot &snapshot) {
    sessionController_.handlePrinterSnapshot(snapshot);
}

void DeviceManager::connectDevice(const QString &port) {
    sessionController_.connectDevice(port);
}

void DeviceManager::disconnectDevice() {
    sessionController_.disconnectDevice();
}

void DeviceManager::requestDeviceInfo() {
    sessionController_.requestDeviceInfo();
}

bool DeviceManager::isPrinterClassDevicePresent() const {
    return sessionController_.isPrinterClassDevicePresent();
}

void DeviceManager::attachPrinterClassDevice(
    const PrinterProtocol::UsbPrinterDevice &device) {
    sessionController_.attachPrinterClassDevice(device);
}

void DeviceManager::detachPrinterClassDevice(bool notify) {
    sessionController_.detachPrinterClassDevice(notify);
}

QString DeviceManager::currentPrinterPath() const {
    return sessionController_.currentPrinterPath();
}

std::optional<PrinterProductProfile>
DeviceManager::currentPrinterProductProfile() const {
    return sessionController_.currentPrinterProductProfile();
}

bool DeviceManager::currentPrinterSupportsMediaCatalog() const {
    return sessionController_.currentPrinterSupportsMediaCatalog();
}

bool DeviceManager::currentPrinterSupportsDisplayConfiguration() const {
    return sessionController_.currentPrinterSupportsDisplayConfiguration();
}

bool DeviceManager::currentPrinterSupportsOverlayMetrics() const {
    return sessionController_.currentPrinterSupportsOverlayMetrics();
}

bool DeviceManager::firmwareFlashAllowedForCurrentDevice(
    QString *errorMessage) const {
    return sessionController_.firmwareFlashAllowedForCurrentDevice(
        errorMessage);
}

QString DeviceManager::printerUnavailableStatusText() const {
    return sessionController_.printerUnavailableStatusText();
}

QString DeviceManager::printerMutationUnavailableStatusText() const {
    return sessionController_.printerMutationUnavailableStatusText();
}

QString DeviceManager::firmwareExclusiveStatusText() const {
    return sessionController_.firmwareExclusiveStatusText();
}

void DeviceManager::resumePrinterSessionAfterRetryCacheValidation() {
    sessionController_.resumePrinterSessionAfterRetryCacheValidation();
}

void DeviceManager::requirePrinterRecovery(const QString &message) {
    sessionController_.requirePrinterRecovery(message);
}

bool DeviceManager::completePrinterRecoveryAfterRemoval(
    const QString &currentDeviceIdentity,
    quint16 currentProductId) {
    return sessionController_.completePrinterRecoveryAfterRemoval(
        currentDeviceIdentity, currentProductId);
}

QString DeviceManager::normalizedOperationId(const QString &requestedId) const {
    return operationCoordinator_.normalizedOperationId(requestedId);
}

void DeviceManager::cleanupMediaRuntimeStaging() {
    operationCoordinator_.cleanupMediaRuntimeStaging();
}

void DeviceManager::cleanupDeviceMediaOutbox() {
    operationCoordinator_.initializeDeviceMediaOutbox();
}

bool DeviceManager::watchArtifactOwner(
    const QString &ownerUniqueName) {
    if (!artifactOwnerWatcher_ ||
        !tryx::DeviceMediaArtifactStore::isValidDbusUniqueName(
            ownerUniqueName)) {
        return false;
    }
    if (!artifactOwnerWatcher_->watchedServices().contains(
            ownerUniqueName)) {
        artifactOwnerWatcher_->addWatchedService(ownerUniqueName);
    }
    QDBusConnectionInterface *interface =
        QDBusConnection::sessionBus().interface();
    if (!interface) {
        return false;
    }
    const QDBusReply<bool> registered =
        interface->isServiceRegistered(ownerUniqueName);
    return registered.isValid() && registered.value();
}

void DeviceManager::handleArtifactOwnerUnregistered(
    const QString &ownerUniqueName) {
    operationCoordinator_.handleArtifactOwnerUnregistered(
        operationContext(), ownerUniqueName);
}

void DeviceManager::sweepDeviceMediaArtifacts() {
    operationCoordinator_.sweepDeviceMediaArtifacts(operationContext());
}

TryxRuntimeOperationsSnapshot DeviceManager::operationSnapshot() const {
    return operationCoordinator_.operationSnapshot();
}

bool DeviceManager::prepareRuntimeDowngradeV10(
    QString *mode, QString *errorName, QString *errorMessage) {
    if (mode) {
        mode->clear();
    }
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto setFailure = [errorName, errorMessage](
                                const QString &message) {
        if (errorName) {
            *errorName = QStringLiteral(
                "org.tryx.Panorama.Error.DowngradeV10Blocked");
        }
        if (errorMessage) {
            *errorMessage = message;
        }
    };
    if (QThread::currentThread() != thread()) {
        setFailure(tr(
            "Runtime downgrade preparation must run on the runtime thread"));
        return false;
    }
    if (runtimeDowngradeV10Prepared_) {
        if (runtimeDowngradeV10Mode_.isEmpty()) {
            setFailure(tr(
                "Runtime downgrade preparation is already in progress"));
            return false;
        }
        if (mode) {
            *mode = runtimeDowngradeV10Mode_;
        }
        return true;
    }

    // This latch is published before inspecting any mutable runtime state.
    // All device entry points execute on this thread, so no later request can
    // enter between the quiescence check and the durable marker commit.
    runtimeDowngradeV10Prepared_ = true;
    const auto fail = [this, &setFailure](const QString &message) {
        runtimeDowngradeV10Mode_.clear();
        runtimeDowngradeV10Prepared_ = false;
        setFailure(message);
        return false;
    };

    if (firmwareExclusiveActive() ||
        !sessionController_.state().firmwareReleasePendingLeaseId.isEmpty() ||
        sessionController_.state().firmwareRecoveryInterlockActive) {
        return fail(tr(
            "Firmware activity or recovery must finish before preparing a runtime downgrade"));
    }
    const auto operationReadiness =
        operationCoordinator_.runtimeDowngradeAssessment();
    if (operationReadiness.activeOperationPresent) {
        return fail(tr(
            "A device operation is still active; wait for it to finish before preparing a runtime downgrade"));
    }
    if (operationReadiness.pendingOperationPresent) {
        return fail(tr(
            "A device operation is still pending; wait for it to finish before preparing a runtime downgrade"));
    }
    if (operationReadiness.recoveryPending) {
        return fail(tr(
            "Delete or replacement recovery must finish before preparing a runtime downgrade"));
    }
    if (!operationReadiness.retryCacheReady) {
        return fail(tr(
            "Stored retry media is not fully validated for runtime downgrade"));
    }

    if (!sessionController_.overlayConfigurationSupportsDowngradeV10() || !savedLayoutStore_
        || !savedLayoutStore_->writesEnabled()
        || !tryx::configurationVersionIsSupported(savedLayoutStore_->indexPath(), 1024 * 1024, {1})) {
        return fail(tr("Overlay or saved-layout settings require a newer runtime. No configuration was downgraded or removed."));
    }

    if (!operationReadiness.retryCompatible) {
        qWarning().noquote()
            << "Runtime downgrade v10 preparation blocked:"
            << operationReadiness.retryCompatibilityStatus;
        return fail(tr(
            "Stored retry media is not exactly compatible with runtime v10"));
    }
    const QString compatibilityMode =
        operationReadiness.retryCompatibilityStatus ==
                QStringLiteral("SafeEmpty")
        ? QStringLiteral("Empty")
        : operationReadiness.retryCompatibilityStatus ==
                QStringLiteral("SafeFullRetry")
            ? QStringLiteral("FullFrame")
            : QString();
    if (compatibilityMode.isEmpty() || !runtimeDowngradeStore_) {
        return fail(tr(
            "Runtime downgrade preparation could not establish a supported compatibility state"));
    }

    QString identityDetail;
    const auto executableIdentity =
        tryx::RuntimeDowngradeStore::currentExecutableIdentity(
            &identityDetail);
    if (!executableIdentity.isValid()) {
        qWarning().noquote()
            << "Runtime downgrade executable identity failed:"
            << identityDetail;
        return fail(tr(
            "The current runtime executable identity could not be verified"));
    }

    if (!worker_ ||
        (worker_->thread() != QThread::currentThread() &&
         !workerThread_.isRunning())) {
        return fail(tr(
            "Runtime downgrade preparation could not establish a supported compatibility state"));
    }

    // The main-thread latch above prevents new requests. Close the PASE
    // generation gate immediately, then place a blocking fence in the shared
    // worker queue. When it returns, every command accepted before the latch
    // has completed and all legacy/PASE transport timers are stopped. Only
    // this proven quiescent state may be recorded in the durable marker.
    stopKeepalive();
    if (artifactSweepTimer_) {
        artifactSweepTimer_->stop();
    }
    sessionController_.invalidateForRuntimeDowngrade();
    bool workerQuiesced = true;
    if (worker_->thread() == QThread::currentThread()) {
        worker_->quiesceForRuntimeDowngrade(
            sessionController_.state().printerGeneration);
    } else {
        workerQuiesced = QMetaObject::invokeMethod(
            worker_,
            [worker = worker_,
             generation = sessionController_.state().printerGeneration]() {
                worker->quiesceForRuntimeDowngrade(generation);
            },
            Qt::BlockingQueuedConnection);
    }
    const auto failAfterWorkerFence =
        [this, &setFailure](const QString &message) {
            runtimeDowngradeV10Mode_.clear();
            setFailure(message);
            emit runtimeDowngradeV10PreparedForExit();
            return false;
        };
    if (!workerQuiesced) {
        return failAfterWorkerFence(tr(
            "The device transport could not be quiesced for runtime downgrade; the runtime is stopping safely"));
    }

    const auto finalOperationReadiness =
        operationCoordinator_.runtimeDowngradeAssessment();
    if (!finalOperationReadiness.retryCacheReady ||
        !finalOperationReadiness.retryCompatible ||
        finalOperationReadiness.retryCompatibilityStatus !=
            operationReadiness.retryCompatibilityStatus) {
        return failAfterWorkerFence(tr(
            "Stored retry media is not exactly compatible with runtime v10"));
    }
    QString finalIdentityDetail;
    const auto finalExecutableIdentity =
        tryx::RuntimeDowngradeStore::currentExecutableIdentity(
            &finalIdentityDetail);
    if (!finalExecutableIdentity.isValid() ||
        finalExecutableIdentity.path != executableIdentity.path ||
        finalExecutableIdentity.device != executableIdentity.device ||
        finalExecutableIdentity.inode != executableIdentity.inode ||
        finalExecutableIdentity.size != executableIdentity.size ||
        finalExecutableIdentity.sha256 != executableIdentity.sha256) {
        qWarning().noquote()
            << "Runtime downgrade executable identity changed after transport quiesce:"
            << finalIdentityDetail;
        return failAfterWorkerFence(tr(
            "The current runtime executable identity could not be verified"));
    }

    const auto persisted = runtimeDowngradeStore_->persist(
        compatibilityMode, finalOperationReadiness.retryStoreRevision,
        finalExecutableIdentity);
    if (!persisted.ok) {
        qWarning().noquote()
            << "Runtime downgrade marker commit failed:"
            << persisted.detail;
        if (persisted.commitMayExist) {
            // Once rename may have committed the authoritative marker, the
            // mutation latch is irreversible in this process. The caller
            // still receives a failure, while the runtime exits cleanly and
            // the startup gate resolves the durable outcome on next launch.
            return failAfterWorkerFence(tr(
                "The runtime downgrade marker commit outcome is uncertain; the runtime is stopping safely"));
        }
        return failAfterWorkerFence(tr(
            "The runtime downgrade marker could not be committed durably; the runtime is stopping safely"));
    }

    runtimeDowngradeV10Mode_ = compatibilityMode;
    if (mode) {
        *mode = compatibilityMode;
    }
    qInfo().noquote()
        << "Runtime downgrade v10 prepared with compatibility state"
        << compatibilityMode;
    emit runtimeDowngradeV10PreparedForExit();
    return true;
}

QString DeviceManager::supportSnapshotV1(
    const TryxRuntimeSnapshot &connection) const {
    const auto operationState = operationCoordinator_.supportState();
    tryx::SupportSnapshotSourceV1 source;
    source.runtimeVersion = QCoreApplication::applicationVersion();
    if (source.runtimeVersion.isEmpty()) {
        source.runtimeVersion = QStringLiteral("unavailable");
    }
    source.runtimeApiVersion = tryxRuntimeApiVersion();
    source.connection = connection;
    source.physicalGeneration = sessionController_.state().printerGeneration;
    source.recoveryRequired =
        sessionController_.state().printerRecoveryRequired;
    source.firmwareRecoveryInterlockActive =
        sessionController_.state().firmwareRecoveryInterlockActive;
    source.mediaCatalogEntryCount = operationState.mediaCatalogEntryCount;
    source.artifactCount = operationState.artifactCount;
    source.operationCount = operationState.operationCount;
    source.retryCandidatePresent = operationState.retryCandidatePresent;
    source.retryDispatchPresent = operationState.retryDispatchPresent;
    source.retryCleanupPendingCount =
        operationState.retryCleanupPendingCount;
    source.deleteRecoveryPresent = operationState.deleteRecoveryPresent;
    source.replaceRecoveryPresent = operationState.replaceRecoveryPresent;
    source.operations = operationState.recentOperations;
    return tryx::buildSupportSnapshotV1(source);
}

TryxRuntimeOperationInfo DeviceManager::operationInfo(
    const QString &operationId) const {
    return operationCoordinator_.operationInfo(operationId);
}

TryxRuntimeOperationInfo DeviceManager::activeOperationInfo() const {
    return operationCoordinator_.activeOperationInfo();
}

QStringList DeviceManager::metricsCapabilities() const {
    return tryxMetricsCatalog();
}

void DeviceManager::publishMetricsState() {
    sessionController_.publishMetricsState();
}

void DeviceManager::publishDisplayState() {
    sessionController_.publishDisplayState();
}

void DeviceManager::updateDisplayState(
    const PrinterProtocol::PaseDisplayState &state,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    sessionController_.updateDisplayState(state, overlay);
}

TryxRuntimeMediaCatalogSnapshot DeviceManager::mediaCatalogSnapshot() const {
    return operationCoordinator_.mediaCatalogSnapshot();
}

TryxRuntimeDeviceCapabilitiesV1 DeviceManager::deviceCapabilitiesV1(
    quint64 connectionRevision) const {
    return sessionController_.deviceCapabilitiesV1(connectionRevision);
}

TryxRuntimeDeviceSpecificationsV1 DeviceManager::deviceSpecificationsV1(
    const TryxRuntimeSnapshot &connection) const {
    return sessionController_.deviceSpecificationsV1(connection);
}

QString DeviceManager::mediaCatalogDirectory() const {
    return operationCoordinator_.mediaCatalogDirectory();
}

QString DeviceManager::mediaThumbnailPath(
    const QString &thumbnailKey) const {
    return operationCoordinator_.mediaThumbnailPath(thumbnailKey);
}

void DeviceManager::loadMediaCatalogStore() {
    operationCoordinator_.loadMediaCatalogStore();
}

void DeviceManager::loadSavedLayoutsStore() {
    savedLayoutsStoreLoaded_ = true;
    savedLayoutsFailureDetail_.clear();
    const tryx::SavedLayoutStore::LoadResult result =
        savedLayoutStore_->load();
    if (!result.writesEnabled) {
        savedLayoutsFailureDetail_ = result.detail.isEmpty()
            ? tr("Saved layouts could not be loaded safely")
            : result.detail.left(512);
        qWarning().noquote() << savedLayoutsFailureDetail_;
    }
}

bool DeviceManager::currentSavedLayoutsContext(
    QString *deviceIdentity, QString *productId) const {
    if (deviceIdentity) {
        deviceIdentity->clear();
    }
    if (productId) {
        productId->clear();
    }
    if (!sessionController_.state().connected ||
        !sessionController_.state().printerClassConnected ||
        sessionController_.state().printerGeneration == 0 ||
        currentPrinterPath().isEmpty()) {
        return false;
    }
    const QString identity = sessionController_.state().printerDeviceSerial;
    const QString product =
        printerProductIdString(sessionController_.state().printerProductId);
    const auto profile = currentPrinterProductProfile();
    const bool valid =
        tryxSavedLayoutDeviceIdentityIsCanonical(identity) &&
        tryxSavedLayoutProductIdIsSupported(product) && profile &&
        (profile->productId == 0x1011 ||
         profile->productId == 0x1021) &&
        profile->mediaCatalogSupported &&
        profile->displayConfigurationSupported &&
        profile->overlayMetricsSupported;
    if (!valid) {
        return false;
    }
    if (deviceIdentity) {
        *deviceIdentity = identity;
    }
    if (productId) {
        *productId = product;
    }
    return true;
}

bool DeviceManager::buildSavedLayoutMediaProof(
    const TryxRuntimeApplyRequest &request,
    QList<TryxRuntimeSavedMediaRefV1> *proof,
    QString *errorMessage) const {
    if (proof) {
        proof->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    QString deviceIdentity;
    QString productId;
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    const TryxRuntimeMediaCatalogSnapshot mediaCatalog =
        operationCoordinator_.mediaCatalogSnapshot();
    if (!proof ||
        !currentSavedLayoutsContext(&deviceIdentity, &productId) ||
        mediaCatalog.deviceIdentity != deviceIdentity) {
        return fail(tr(
            "The authoritative media catalog is unavailable for this device"));
    }
    const bool split = request.screenMode ==
        QStringLiteral("Screen Splitting");
    const auto profile = currentPrinterProductProfile();
    if ((split && (!profile || !profile->splitAreaMediaSupported)) ||
        request.media.size() != (split ? 2 : 1)) {
        return fail(tr(
            "The saved layout media count is not supported by this device"));
    }

    QSet<QString> seenNames;
    for (const QString &name : request.media) {
        if (name.isEmpty() || seenNames.contains(name)) {
            return fail(tr(
                "The saved layout media selection is ambiguous"));
        }
        seenNames.insert(name);
        QList<TryxRuntimeMediaEntry> matches;
        for (const TryxRuntimeMediaEntry &entry : mediaCatalog.entries) {
            if (entry.name == name) {
                matches.append(entry);
            }
        }
        if (matches.size() != 1) {
            return fail(tr(
                "The saved layout media is missing or ambiguous in the current catalog"));
        }
        const TryxRuntimeMediaEntry &entry = matches.constFirst();
        if (!isSha256Hex(entry.mediaId) || entry.size == 0 ||
            (entry.source != 1U && entry.source != 2U)) {
            return fail(tr(
                "The saved layout media identity is incomplete"));
        }
        TryxRuntimeSavedMediaRefV1 reference;
        reference.mediaId = entry.mediaId;
        reference.name = entry.name;
        reference.size = entry.size;
        reference.source = entry.source;
        reference.readOnly = entry.readOnly;
        proof->append(reference);
    }
    return true;
}

void DeviceManager::updateMediaCatalog(
    const QList<PrinterProtocol::MediaFile> &mediaFiles) {
    operationCoordinator_.updateMediaCatalog(
        operationContext(), mediaFiles);
}

void DeviceManager::clearMediaCatalogView() {
    operationCoordinator_.clearMediaCatalogView();
}

void DeviceManager::loadRuntimePresentationPreferences() {
    presentationPreferences_ = {};
    if (!runtimePresentationPreferencesStore_) {
        qWarning().noquote()
            << "The runtime presentation preferences store is unavailable;"
            << "using Celsius and 24H";
    } else {
        const auto result = runtimePresentationPreferencesStore_->load();
        presentationPreferences_ = result.preferences;
        presentationPreferences_.revision = 1;
        if (!result.detail.isEmpty()) {
            qWarning().noquote() << result.detail;
        }
    }
    const TryxRuntimePresentationPreferencesV1 loaded =
        presentationPreferences_;
    worker_->publishPresentationPreferences(loaded);
    if (worker_->thread() == QThread::currentThread()) {
        worker_->applyPublishedPresentationPreferences();
    } else {
        QMetaObject::invokeMethod(
            worker_,
            [worker = worker_]() {
                worker->applyPublishedPresentationPreferences();
            },
            Qt::QueuedConnection);
    }
}

void DeviceManager::loadPaseMetricsConfig() {
    sessionController_.loadPaseMetricsConfig();
}

bool DeviceManager::persistPaseMetricsConfiguration(
    const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
    QString *errorMessage) {
    return sessionController_.persistPaseMetricsConfiguration(overlay, enabled,
                                                              errorMessage);
}

PrinterProtocol::PaseOverlayConfig
DeviceManager::persistedPaseOverlayForDevice(
    const QString &deviceSerial) const {
    return sessionController_.persistedPaseOverlayForDevice(deviceSerial);
}

void DeviceManager::rejectOperation(const QString &operationId,
                                    const QString &kind,
                                    const QString &subject,
                                    const QString &category,
                                    const QString &message) {
    operationCoordinator_.rejectOperation(
        operationId, kind, subject, category, message,
        sessionController_.state().printerGeneration);
}

void DeviceManager::rejectSavedLayoutApplyOperation(
    const QString &operationId, const QString &subject,
    const QString &category, const QString &message) {
    operationCoordinator_.rejectOperation(
        operationId, QStringLiteral("SavedLayoutApply"), subject, category,
        message, sessionController_.state().printerGeneration,
        QStringLiteral("NotStarted"));
}

QString DeviceManager::queueStageDeviceMediaOperation(
    const QString &requestedOperationId, const QString &mediaId,
    const QString &ownerUniqueName) {
    return operationCoordinator_.queueStageDeviceMediaOperation(
        operationContext(), requestedOperationId, mediaId,
        ownerUniqueName);
}

TryxRuntimeDeviceMediaArtifact
DeviceManager::claimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.claimDeviceMediaArtifact(
        operationContext(), operationId, artifactId, ownerUniqueName,
        errorMessage);
}

TryxRuntimeDeviceMediaMetadataV1
DeviceManager::deviceMediaMetadataV1(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) const {
    return operationCoordinator_.deviceMediaMetadataV1(
        artifactId, leaseId, ownerUniqueName, errorMessage);
}

bool DeviceManager::renewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.renewDeviceMediaArtifactLease(
        operationContext(), artifactId, leaseId, ownerUniqueName,
        errorMessage);
}

bool DeviceManager::releaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    return operationCoordinator_.releaseDeviceMediaArtifact(
        operationContext(), artifactId, leaseId, ownerUniqueName,
        errorMessage);
}

QString DeviceManager::queueRecoveredMediaUploadOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaTransform &transform) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName,
        tryxFullFrameMediaPreparationProfile(transform),
        false, QString(),
        TryxRuntimeApplyRequest{});
}

QString DeviceManager::
queueRecoveredMediaUploadWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, false, QString(),
        TryxRuntimeApplyRequest{});
}

QString DeviceManager::queueReplaceDeviceMediaOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform,
    const QString &ownerUniqueName) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName,
        tryxFullFrameMediaPreparationProfile(transform), true,
        originalMediaId, request);
}

QString DeviceManager::
queueReplaceDeviceMediaWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    const QString &ownerUniqueName) {
    return queueRecoveredOperation(
        requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, true,
        originalMediaId, request);
}

QString DeviceManager::queueRecoveredOperation(
    const QString &requestedOperationId,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName,
    const TryxRuntimeMediaPreparationProfileV1 &profile, bool replace,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &applyRequest) {
    return operationCoordinator_.queueRecoveredOperation(
        operationContext(), requestedOperationId, artifactId, leaseId,
        ownerUniqueName, profile, replace, originalMediaId,
        applyRequest);
}

QString DeviceManager::queueUploadOperation(
    const QString &requestedOperationId,
    const QString &localPath, bool applyAfterUpload,
    const TryxRuntimeApplyRequest &applyRequest,
    bool updateMetrics, bool ensureExisting,
    const TryxRuntimeMediaTransform &transform) {
    return queueUploadOperationWithPreparationProfile(
        requestedOperationId, localPath, applyAfterUpload,
        applyRequest, updateMetrics, ensureExisting,
        tryxFullFrameMediaPreparationProfile(transform));
}

QString DeviceManager::queueUploadWithPreparationProfileOperation(
    const QString &requestedOperationId,
    const QString &localPath,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return queueUploadOperationWithPreparationProfile(
        requestedOperationId, localPath, false,
        TryxRuntimeApplyRequest{}, false, false, profile);
}

QString DeviceManager::queueUploadOperationWithPreparationProfile(
    const QString &requestedOperationId,
    const QString &localPath, bool applyAfterUpload,
    const TryxRuntimeApplyRequest &applyRequest,
    bool updateMetrics, bool ensureExisting,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return operationCoordinator_.queueUploadOperation(
        operationContext(), requestedOperationId, localPath,
        applyAfterUpload, applyRequest, updateMetrics, ensureExisting,
        profile);
}

QString DeviceManager::queueEnsureMediaAndApplyOperation(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &applyRequest,
    const TryxRuntimeMediaTransform &transform) {
    return queueUploadOperation(operationId, localPath, true,
                                applyRequest, true, true, transform);
}

QString DeviceManager::queueUploadWithBadgesOperation(const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyWithBadgesV1 &request, bool ensureExisting, const TryxRuntimeMediaTransform &transform) {
    if (request.schemaVersion != 1) return {};
    return operationCoordinator_.queueUploadOperation(operationContext(), operationId, localPath, true,
        request.request, true, ensureExisting, tryxFullFrameMediaPreparationProfile(transform), request.badges);
}

QString DeviceManager::queueDeleteMediaOperation(
    const QString &requestedOperationId,
    const QStringList &fileNames) {
    return operationCoordinator_.queueDeleteMediaOperation(
        operationContext(), requestedOperationId, fileNames);
}

QString DeviceManager::queueApplyOperation(
    const QString &requestedOperationId,
    const TryxRuntimeApplyRequest &request, bool updateMetrics,
    const QString &proofDeviceIdentity,
    const QList<TryxRuntimeSavedMediaRefV1> &proof,
    bool savedLayoutApply) {
    return operationCoordinator_.queueApplyOperation(
        operationContext(), requestedOperationId, request, updateMetrics,
        proofDeviceIdentity, proof, savedLayoutApply);
}

QString DeviceManager::queueApplyWithBadgesOperation(const QString &operationId,
                                                    const TryxRuntimeApplyWithBadgesV1 &request) {
    if (request.schemaVersion != 1) return {};
    return operationCoordinator_.queueApplyOperation(operationContext(), operationId,
        request.request, false, {}, {}, false, request.badges);
}

QString DeviceManager::queueSavedLayoutApplyOperation(
    const QString &requestedOperationId, const QString &layoutId,
    quint64 expectedLayoutRevision,
    const TryxRuntimeApplyRequest &currentDraft) {
    return queueSavedLayoutApplyInternal(requestedOperationId, layoutId, expectedLayoutRevision, currentDraft, std::nullopt);
}

QString DeviceManager::queueSavedLayoutApplyWithBadgesOperation(const QString &operationId, const QString &layoutId,
    quint64 expectedLayoutRevision, const TryxRuntimeApplyWithBadgesV1 &currentDraft) {
    if (currentDraft.schemaVersion != 1) return {};
    return queueSavedLayoutApplyInternal(operationId, layoutId, expectedLayoutRevision, currentDraft.request, currentDraft.badges);
}

QString DeviceManager::queueSavedLayoutApplyInternal(const QString &requestedOperationId, const QString &layoutId,
    quint64 expectedLayoutRevision, const TryxRuntimeApplyRequest &currentDraft,
    const std::optional<TryxRuntimeOverlayBadgesV1> &badges) {
    const QString operationId =
        normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    if (!operationCoordinator_.operationInfo(operationId).id.isEmpty()) {
        return operationCoordinator_.repeatSavedLayoutApplyOperation(operationContext(), operationId,
            currentDraft, badges, layoutId, expectedLayoutRevision);
    }
    const auto reject =
        [this, &operationId, &layoutId](
            const QString &category, const QString &message) {
            rejectSavedLayoutApplyOperation(
                operationId, layoutId, category, message);
            return operationId;
        };
    if (runtimeDowngradeV10Prepared_) {
        return reject(
            QStringLiteral("DowngradePrepared"),
            tr("Saved layout Apply is blocked after runtime downgrade preparation"));
    }

    const TryxRuntimeSavedLayoutsSnapshotV2 snapshot = savedLayoutsSnapshotV2();
    if (snapshot.status != QStringLiteral("Ready")) {
        return reject(
            snapshot.status == QStringLiteral("Unsupported")
                ? QStringLiteral("UnsupportedProduct")
                : QStringLiteral("SavedLayoutsUnavailable"),
            snapshot.diagnostic.isEmpty()
                ? tr("Saved layouts are unavailable for this device")
                : snapshot.diagnostic);
    }

    auto found = std::find_if(
        snapshot.layouts.cbegin(), snapshot.layouts.cend(),
        [&layoutId](const TryxRuntimeSavedLayoutV2 &layout) {
            return layout.layoutId == layoutId;
    });
    if (found == snapshot.layouts.cend()) {
        const QList<TryxRuntimeSavedLayoutV1> allLayouts =
            savedLayoutStore_->layouts();
        const auto foreign = std::find_if(
            allLayouts.cbegin(), allLayouts.cend(),
            [&layoutId](const TryxRuntimeSavedLayoutV1 &layout) {
                return layout.layoutId == layoutId;
            });
        if (foreign != allLayouts.cend()) {
            return reject(
                QStringLiteral("DeviceIdentityChanged"),
                tr("The saved layout belongs to another connected device"));
        }
        return reject(
            QStringLiteral("SavedLayoutMissing"),
            tr("The saved layout no longer exists"));
    }
    if (found->revision != expectedLayoutRevision) {
        return reject(
            QStringLiteral("SavedLayoutRevisionConflict"),
            tr("The saved layout changed; load it again before applying"));
    }

    QList<TryxRuntimeSavedMediaRefV1> proof;
    QString validationError;
    if (!buildSavedLayoutMediaProof(
            currentDraft, &proof, &validationError)) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            validationError.isEmpty()
                ? tr("The saved layout media is unavailable")
                : validationError);
    }
    if (!badges && tryxOverlayBadgesHaveCustomText(found->badges)) {
        return reject(QStringLiteral("UnsupportedConfiguration"), tr("This saved layout requires the V2 interface"));
    }
    TryxRuntimeSavedLayoutV2 candidate = *found;
    candidate.media = proof;
    candidate.request = currentDraft;
    candidate.badges = badges.value_or(TryxRuntimeOverlayBadgesV1{});
    if (!tryx::SavedLayoutStore::layoutIsCanonical(
            candidate, &validationError)) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            validationError.isEmpty()
                ? tr("The current saved layout draft is invalid")
                : validationError);
    }

    return operationCoordinator_.queueApplyOperation(operationContext(),
        operationId, currentDraft, false, snapshot.deviceIdentity, proof, true, badges, layoutId, expectedLayoutRevision);
}

QString DeviceManager::queueCacheCleanupOperation(
    const QString &requestedOperationId, QString *errorName,
    QString *errorMessage) {
    return operationCoordinator_.queueCacheCleanupOperation(
        operationContext(), requestedOperationId, errorName,
        errorMessage);
}

QString DeviceManager::queueMetricsConfigOperation(
    const QString &requestedOperationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    return operationCoordinator_.queueMetricsConfigOperation(
        operationContext(), requestedOperationId, request);
}

QString DeviceManager::retryOperation(
    const QString &sourceOperationId,
    const QString &requestedNewOperationId) {
    return operationCoordinator_.retryOperation(
        operationContext(), sourceOperationId, requestedNewOperationId);
}

void DeviceManager::cancelOperation(const QString &operationId) {
    operationCoordinator_.cancelOperation(operationContext(), operationId);
}

void DeviceManager::cancelForegroundForGenerationChange(
    const QString &message) {
    operationCoordinator_.cancelForegroundForGenerationChange(
        operationContext(), message);
}


bool DeviceManager::releasePrinterPreparationPath(
    const QString &path) {
    return operationCoordinator_.releasePrinterPreparationPath(path);
}

void DeviceManager::loadReplaceJournal() {
    operationCoordinator_.loadReplaceJournal();
}

void DeviceManager::resumePendingReplaceReconciliation() {
    operationCoordinator_.resumePendingReplaceReconciliation(
        operationContext());
}

void DeviceManager::loadDeleteIntent() {
    operationCoordinator_.loadDeleteIntent();
}

void DeviceManager::resumePendingDeleteReconciliation() {
    operationCoordinator_.resumePendingDeleteReconciliation(
        operationContext());
}

bool DeviceManager::retryCacheStoreBlocksMutations() const {
    return operationCoordinator_.retryCacheStoreBlocksMutations();
}

bool DeviceManager::retryCacheStartupSessionGateActive() const {
    return operationCoordinator_.retryCacheStartupSessionGateActive();
}

bool DeviceManager::retryCacheRestrictedRecoveryActive() const {
    return operationCoordinator_.retryCacheRestrictedRecoveryActive();
}

bool DeviceManager::retryCacheMutationGateActive() const {
    return operationCoordinator_.retryCacheMutationGateActive(
        operationContext());
}

void DeviceManager::loadRetryCache() {
    operationCoordinator_.loadRetryCache(operationContext());
}

void DeviceManager::handleRetryCacheArtifactValidation(
    const QString &validationToken, bool valid,
    bool cancelled, qint64 actualSize,
    const QString &actualSha256, quint64 actualDevice,
    quint64 actualInode, const QString &message) {
    operationCoordinator_.handleRetryCacheArtifactValidation(
        operationContext(), validationToken, valid, cancelled,
        actualSize, actualSha256, actualDevice, actualInode, message);
}


void DeviceManager::startRetryCacheReadOnlyReconciliationIfReady() {
    operationCoordinator_.startRetryCacheReadOnlyReconciliationIfReady(
        operationContext());
}

void DeviceManager::promoteRestrictedSessionAfterProof() {
    sessionController_.promoteRestrictedSessionAfterProof();
}


void DeviceManager::setBrightness(int value) {
    const int boundedValue = qBound(0, value, 100);
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.display.brightnessPresent = true;
        request.display.brightness = boundedValue;
        queueApplyOperation(QString(), request);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
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
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
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
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }

    if (!sessionController_.state().connected) {
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
    if (firmwareExclusiveActive()) {
        return;
    }
    if (isPrinterClassDevicePresent()) {
        if (!currentPrinterSupportsOverlayMetrics()) {
            emit deviceError(
                tr("Overlay metrics are not supported for USB product %1")
                    .arg(printerProductIdString(
                        sessionController_.state().printerProductId)));
            return;
        }
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty() ||
            !sessionController_.state().printerDisplaySessionActive ||
            retryCacheMutationGateActive() ||
            sessionController_.state().printerRecoveryRequired ||
            sessionController_.state().printerDisplaySessionLost ||
            !operationCoordinator_.activeOperationId().isEmpty()) {
            return;
        }
        emit requestPrinterSysinfo(
            devicePath, labels, values, units,
            sessionController_.state().printerGeneration);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestSysinfo(labels, values, units);
}

void DeviceManager::setRotation(int degrees) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Rotation is not supported on printer-class firmware yet."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestRotation(degrees);
}

void DeviceManager::rebootDevice() {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Reboot is not supported on printer-class firmware yet."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestReboot();
}

void DeviceManager::deleteMedia(const QStringList &files) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        emit uploadStatus(
            tr("Media deletion is disabled because USB file_remove has no dedicated response."));
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    emit requestDeleteMedia(files);
}

void DeviceManager::uploadMedia(const QString &localPath) {
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        queueUploadOperation(QString(), localPath, false);
        return;
    }
    if (retryCacheMutationGateActive()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (!sessionController_.state().connected) {
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestUploadMedia(localPath);
}

void DeviceManager::refreshMediaList() {
    if (runtimeDowngradeV10Prepared_) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (isPrinterClassDevicePresent()) {
        if (!currentPrinterSupportsMediaCatalog()) {
            emit deviceError(
                tr("Media catalog refresh is not supported for USB product %1")
                    .arg(printerProductIdString(
                        sessionController_.state().printerProductId)));
            return;
        }
        const QString devicePath = currentPrinterPath();
        if (devicePath.isEmpty()) {
            emit deviceError(printerUnavailableStatusText());
            return;
        }
        if (retryCacheMutationGateActive() ||
            sessionController_.state().printerRecoveryRequired ||
            sessionController_.state().printerDisplaySessionLost) {
            emit deviceError(printerMutationUnavailableStatusText());
            return;
        }
        if (!operationCoordinator_.activeOperationId().isEmpty()) {
            emit deviceError(
                tr("FileList refresh is deferred while operation %1 is active")
                    .arg(operationCoordinator_.activeOperationId()));
            return;
        }
        emit requestPrinterRefreshMedia(
            devicePath, QString(),
            sessionController_.state().printerGeneration);
        return;
    }
    if (!sessionController_.state().connected) {
        emit mediaListUpdated({});
        emit uploadStatus(
            tr("Device not connected. Use Auto connection or reconnect USB."));
        return;
    }
    emit requestRefreshMedia();
}

void DeviceManager::startKeepalive(int intervalSec) {
    sessionController_.startKeepalive(intervalSec);
}

void DeviceManager::stopKeepalive() {
    sessionController_.stopKeepalive();
}

PrinterSessionController::Callbacks DeviceManager::sessionCallbacks() {
    PrinterSessionController::Callbacks callbacks;
    callbacks.runtimeDowngradePrepared = [this]() {
        return runtimeDowngradeV10Prepared_;
    };
    callbacks.workerAvailable = [this]() {
        return worker_ != nullptr;
    };
    callbacks.workerRunning = [this]() {
        return worker_ && workerThread_.isRunning();
    };
    callbacks.activeOperationId = [this]() {
        return operationCoordinator_.activeOperationId();
    };
    callbacks.retryCacheStoreBlocksMutations = [this]() {
        return retryCacheStoreBlocksMutations();
    };
    callbacks.retryCacheStartupSessionGateActive = [this]() {
        return retryCacheStartupSessionGateActive();
    };
    callbacks.retryCacheRestrictedRecoveryActive = [this]() {
        return retryCacheRestrictedRecoveryActive();
    };
    callbacks.retryCacheMutationGateActive = [this]() {
        return retryCacheMutationGateActive();
    };
    callbacks.retryCacheValidationPending = [this]() {
        return operationCoordinator_.retryCacheValidationPending();
    };
    callbacks.hasUnresolvedRetryOutcomeForFirmware = [this]() {
        return operationCoordinator_.hasUnresolvedRetryOutcomeForFirmware();
    };
    callbacks.hasPendingDeleteRecovery = [this]() {
        return operationCoordinator_.hasPendingDeleteRecovery();
    };
    callbacks.hasPendingReplaceRecovery = [this]() {
        return operationCoordinator_.hasPendingReplaceRecovery();
    };
    callbacks.sessionStarted = [this]() {
        return operationCoordinator_.handleSessionStarted(operationContext());
    };
    callbacks.sessionLostBeforeStateChange = [this]() {
        return operationCoordinator_.handleSessionLostBeforeStateChange(operationContext());
    };
    callbacks.clearMediaCatalogView = [this]() {
        clearMediaCatalogView();
    };
    callbacks.startRetryCacheReadOnlyReconciliationIfReady = [this]() {
        startRetryCacheReadOnlyReconciliationIfReady();
    };
    callbacks.resumePendingDeleteReconciliation = [this]() {
        resumePendingDeleteReconciliation();
    };
    callbacks.resumePendingReplaceReconciliation = [this]() {
        resumePendingReplaceReconciliation();
    };
    callbacks.cancelForegroundForGenerationChange = [this](const QString &message) {
        cancelForegroundForGenerationChange(message);
    };
    callbacks.completeRetryRecoveryAfterRemoval = [this](const QString &identity, quint16 productId) {
        PrinterOperationContext context = operationContext();
        context.deviceIdentity = identity.trimmed();
        context.productId = productId;
        return operationCoordinator_.completeRetryRecoveryAfterRemoval(context);
    };
    return callbacks;
}
