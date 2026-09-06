#include "printersessioncontroller.h"
#include "configurationformatbackup.h"
#include "devicemanagermessages.h"
#include "paseoverlayconfig.h"
#include "pasemetricsconfigstore.h"
#include "printerlifecycle_p.h"
#include <QThread>
#include <QFileInfo>
#include <panorama/device.hpp>
#include <utility>

using namespace tryx::pase_overlay_config;
using namespace tryx::printer_lifecycle;

namespace {
TryxRuntimeDisplayState projectDisplayState(const PrinterProtocol::PaseDisplayState &raw,
    const PrinterProtocol::PaseOverlayConfig &overlay, const QString &identity, bool coherent = false) {
    TryxRuntimeDisplayState display;
    display.deviceSerial = identity;
    display.valid = true;
    display.backlightEnabled = raw.backlightEnabled;
    display.brightness = raw.brightness;
    display.standbyEnabled = raw.standbyEnabled;
    display.standbyMedia = raw.standbyMedia;
    display.mirrorMode = raw.mirrorMode;
    display.waterfallMode = raw.waterfallMode;
    display.screenMode = raw.screenMode;
    display.playMode = raw.playMode;
    display.media = raw.media;
    display.sysinfoLabels = overlay.left.metrics;
    display.settingsBadges = overlay.left.badges;
    display.settingsPosition = overlay.left.verticalPlacement;
    display.settingsColor = paseTextColorName(overlay.left.textColor);
    display.settingsAlign = overlay.left.alignment;
    if (coherent ? raw.screenMode == QStringLiteral("Screen Splitting") : overlay.dualMode) {
        display.sysinfoLabels2 = overlay.right.metrics;
        display.settingsBadges2 = overlay.right.badges;
        display.settingsPosition2 = overlay.right.verticalPlacement;
        display.settingsColor2 = paseTextColorName(overlay.right.textColor);
        display.settingsAlign2 = overlay.right.alignment;
    }
    return display;
}

bool displayAndOverlayAreCoherent(const PrinterProtocol::PaseDisplayState &raw,
    const PrinterProtocol::PaseOverlayConfig &overlay, quint16 productId) {
    return raw.brightness >= 0 && raw.brightness <= 100
        && (raw.screenMode == QStringLiteral("Full Screen") || raw.screenMode == QStringLiteral("Screen Splitting"))
        && (raw.playMode == QStringLiteral("Single") || raw.playMode == QStringLiteral("Loop") || raw.playMode == QStringLiteral("Shuffle"))
        && paseBadgeChoicesAreValid(overlay, productId)
        && (!paseOverlayHasContent(overlay) || overlay.dualMode == (raw.screenMode == QStringLiteral("Screen Splitting")));
}
}

PrinterSessionController::PrinterSessionController(Callbacks callbacks,
                                                   QObject *parent)
    : QObject(parent), callbacks_(std::move(callbacks)),
      keepaliveTimer_(new QTimer(this)),
      paseMetricsConfigStore_(
          std::make_unique<tryx::PaseMetricsConfigStore>()) {
    connect(keepaliveTimer_, &QTimer::timeout, this, [this]() {
        if (!callbacks_.runtimeDowngradePrepared() &&
            !firmwareExclusiveActive() &&
            !state_.firmwareRecoveryInterlockActive &&
            !state_.printerClassConnected) {
            emit requestKeepalive();
        }
    });
}

PrinterSessionController::~PrinterSessionController() = default;

void PrinterSessionController::setPrinterDisplaySessionActive(bool active) {
    if (state_.printerDisplaySessionActive == active) {
        return;
    }
    state_.printerDisplaySessionActive = active;
    if (!active) invalidateDisplaySnapshot();
    emit printerDisplaySessionChanged(active);
    if (active) tryAcceptBootstrapDisplay();
}

bool PrinterSessionController::coherentDisplayContextIsCurrent(quint64 generation,
    const QString &identity, quint16 productId) const {
    return printerResultIsCurrent(generation) && state_.printerDisplaySessionActive
        && !identity.isEmpty() && identity == state_.printerDeviceSerial.trimmed()
        && productId == state_.printerProductId && (productId == 0x1021 || productId == 0x1011);
}

TryxRuntimeDisplaySnapshotV1 PrinterSessionController::displaySnapshotV1(quint64 connectionRevision) const {
    auto snapshot = displaySnapshot_;
    if (!coherentDisplayContextIsCurrent(snapshot.physicalGeneration, displaySnapshotIdentity_, state_.printerProductId)
        || snapshot.productId != printerProductIdString(state_.printerProductId)) {
        const auto revision = snapshot.revision;
        snapshot = {};
        snapshot.revision = revision;
        snapshot.physicalGeneration = state_.printerGeneration;
    }
    snapshot.connectionRevision = connectionRevision;
    return snapshot;
}

void PrinterSessionController::publishDisplaySnapshot(TryxRuntimeDisplaySnapshotV1 snapshot) {
    snapshot.revision = displaySnapshot_.revision + 1;
    snapshot.physicalGeneration = state_.printerGeneration;
    snapshot.productId = state_.printerClassConnected ? printerProductIdString(state_.printerProductId) : QString();
    snapshot.connectionRevision = 0; // Getter stamps the live D-Bus connection context.
    snapshot.display.revision = snapshot.display.valid ? snapshot.revision : 0;
    if (snapshot.status == QStringLiteral("HostAccepted") && !tryxDisplaySnapshotV1IsValid(snapshot)) {
        snapshot.status = QStringLiteral("Unresolved");
        snapshot.acceptedOperationId.clear();
        snapshot.display = {};
        snapshot.badges = {};
    }
    displaySnapshotIdentity_ = state_.printerDeviceSerial.trimmed();
    displaySnapshot_ = std::move(snapshot);
    // This is an invalidation hint, not a state payload. Queue/coalesce it so a
    // subscriber cannot re-enter a half-completed connection transition.
    const quint64 revision = displaySnapshot_.revision;
    QMetaObject::invokeMethod(this, [this, revision]() {
        if (displaySnapshot_.revision == revision) emit displaySnapshotChangedV1(revision);
    }, Qt::QueuedConnection);
}

void PrinterSessionController::invalidateDisplaySnapshot(bool newGeneration) {
    if (newGeneration || displaySnapshot_.physicalGeneration != state_.printerGeneration) bootstrapDisplayAllowed_ = true;
    bootstrapDisplay_.reset();
    pendingDisplayOperationId_.clear();
    pendingDisplayReplacesOverlay_ = false;
    beforeDisplayMutation_ = {};
    publishDisplaySnapshot({});
}

void PrinterSessionController::tryAcceptBootstrapDisplay() {
    if (!bootstrapDisplayAllowed_ || !bootstrapDisplay_
        || !coherentDisplayContextIsCurrent(displaySnapshot_.physicalGeneration, displaySnapshotIdentity_, state_.printerProductId)) return;
    const auto overlay = persistedPaseOverlayForDevice(displaySnapshotIdentity_);
    TryxRuntimeDisplaySnapshotV1 snapshot;
    if (displayAndOverlayAreCoherent(*bootstrapDisplay_, overlay, state_.printerProductId)) {
        snapshot.status = QStringLiteral("HostAccepted");
        snapshot.display = projectDisplayState(*bootstrapDisplay_, overlay, displaySnapshotIdentity_, true);
        snapshot.badges = overlay.badgeChoices;
    } else {
        snapshot.status = QStringLiteral("Unresolved");
    }
    bootstrapDisplayAllowed_ = false;
    bootstrapDisplay_.reset();
    publishDisplaySnapshot(std::move(snapshot));
}

void PrinterSessionController::beginDisplayMutation(const QString &operationId, quint64 generation, bool replacesOverlay) {
    if (!coherentDisplayContextIsCurrent(generation, state_.printerDeviceSerial.trimmed(), state_.printerProductId)
        || operationId.isEmpty() || pendingDisplayOperationId_ == operationId) return;
    beforeDisplayMutation_ = displaySnapshotV1(0);
    pendingDisplayOperationId_ = operationId;
    pendingDisplayReplacesOverlay_ = replacesOverlay;
    bootstrapDisplayAllowed_ = false;
    bootstrapDisplay_.reset();
    TryxRuntimeDisplaySnapshotV1 snapshot;
    snapshot.status = QStringLiteral("Pending");
    publishDisplaySnapshot(std::move(snapshot));
}

bool PrinterSessionController::displayMutationCanPersistOverlay(const QString &operationId, quint64 generation) const {
    // Legacy untracked completions keep their persistence behavior but cannot
    // commit a coherent snapshot. A tracked control-only operation must not
    // reuse the store after an unresolved overlay mutation.
    if (pendingDisplayOperationId_.isEmpty()) return true;
    return pendingDisplayOperationId_ == operationId && generation == displaySnapshot_.physicalGeneration
        && (pendingDisplayReplacesOverlay_ || beforeDisplayMutation_.status == QStringLiteral("HostAccepted"));
}

void PrinterSessionController::finishDisplayMutation(const QString &operationId, quint64 generation, bool provenNoMutation) {
    if (pendingDisplayOperationId_ != operationId || generation != displaySnapshot_.physicalGeneration
        || !coherentDisplayContextIsCurrent(generation, displaySnapshotIdentity_, state_.printerProductId)) return;
    auto snapshot = provenNoMutation ? beforeDisplayMutation_ : TryxRuntimeDisplaySnapshotV1{};
    if (!provenNoMutation) snapshot.status = QStringLiteral("Unresolved");
    pendingDisplayOperationId_.clear();
    pendingDisplayReplacesOverlay_ = false;
    beforeDisplayMutation_ = {};
    publishDisplaySnapshot(std::move(snapshot));
}

void PrinterSessionController::commitDisplayMutation(const QString &operationId, quint64 generation,
    const QString &identity, quint16 productId, const PrinterProtocol::PaseDisplayStateResult &readback,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    if (pendingDisplayOperationId_ != operationId || generation != displaySnapshot_.physicalGeneration
        || identity != displaySnapshotIdentity_ || !coherentDisplayContextIsCurrent(generation, identity, productId)
        || !displayMutationCanPersistOverlay(operationId, generation)
        || !readback.success || !displayAndOverlayAreCoherent(readback.state, overlay, productId)) return;
    TryxRuntimeDisplaySnapshotV1 snapshot;
    snapshot.status = QStringLiteral("HostAccepted");
    snapshot.acceptedOperationId = operationId;
    snapshot.display = projectDisplayState(readback.state, overlay, identity, true);
    snapshot.badges = overlay.badgeChoices;
    pendingDisplayOperationId_.clear();
    pendingDisplayReplacesOverlay_ = false;
    beforeDisplayMutation_ = {};
    publishDisplaySnapshot(std::move(snapshot));
}

void PrinterSessionController::commitMetricsDisplayMutation(const QString &operationId, quint64 generation,
    const QString &identity, quint16 productId, const PrinterProtocol::PaseOverlayConfig &overlay) {
    if (beforeDisplayMutation_.status != QStringLiteral("HostAccepted")) return;
    const auto &display = beforeDisplayMutation_.display;
    PrinterProtocol::PaseDisplayState raw;
    raw.backlightEnabled = display.backlightEnabled;
    raw.brightness = display.brightness;
    raw.standbyEnabled = display.standbyEnabled;
    raw.standbyMedia = display.standbyMedia;
    raw.mirrorMode = display.mirrorMode;
    raw.waterfallMode = display.waterfallMode;
    raw.screenMode = display.screenMode;
    raw.playMode = display.playMode;
    raw.media = display.media;
    commitDisplayMutation(operationId, generation, identity, productId, {true, {}, raw}, overlay);
}

void PrinterSessionController::clearDeviceSpecificationsCache() {
    state_.deviceSpecificationsCache = {};
    state_.deviceSpecificationsDevicePath.clear();
    state_.deviceSpecificationsDeviceIdentity.clear();
    state_.deviceSpecificationsProductId = 0;
    state_.deviceSpecificationsGeneration = 0;
}

void PrinterSessionController::handlePrinterSnapshot(
    const PrinterProtocol::DiscoverySnapshot &snapshot) {
    if (callbacks_.runtimeDowngradePrepared()) {
        return;
    }
    const bool oldPresence = isPrinterClassDevicePresent();
    const bool wasConnected = state_.connected;
    const bool wasPrinterConnected = state_.printerClassConnected;
    const QString oldPath = state_.printerDevicePath;
    const QString oldSerial = state_.printerDeviceSerial;
    const quint16 oldProductId = state_.printerProductId;
    const quint64 previousGeneration = state_.printerGeneration;

    if (state_.printerRecoveryRequired &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        state_.printerRecoveryRemovalObserved = true;
    }
    if (state_.printerDisplaySessionLost &&
        snapshot.state == PrinterProtocol::DiscoveryState::Absent) {
        state_.printerSessionLossRemovalObserved = true;
    }

    if (wasPrinterConnected && state_.printerDisplaySessionActive &&
        !oldSerial.isEmpty()) {
        state_.printerSessionResumePending = true;
        state_.printerSessionResumeSerial = oldSerial;
        state_.printerSessionResumeProductId = oldProductId;
    }
    setPrinterDisplaySessionActive(false);
    if (state_.printerGeneration != previousGeneration ||
        callbacks_.runtimeDowngradePrepared()) {
        return;
    }
    clearDeviceSpecificationsCache();

    callbacks_.cancelForegroundForGenerationChange(
        tryx::DeviceManagerMessages::tr("Printer-class operation stopped "
                                        "because the USB connection changed"));
    if (state_.printerGeneration != previousGeneration ||
        callbacks_.runtimeDowngradePrepared()) {
        return;
    }
    state_.printerSnapshot = snapshot;
    const quint64 generation = ++state_.printerGeneration;
    invalidateDisplaySnapshot(true);
    state_.printerGenerationElapsedTimer.start();
    emit requestCancelPrinterPreparation(state_.printerGeneration);
    if (wasPrinterConnected) {
        emit printerOperationsCancelled();
    }
    if (state_.printerGeneration != generation ||
        callbacks_.runtimeDowngradePrepared()) {
        return;
    }
    if (firmwareExclusiveActive() || state_.firmwareRecoveryInterlockActive) {
        emit requestGenerationGate(state_.printerGeneration, false);
        detachPrinterClassDevice(false);
        state_.legacyProductId.clear();
        state_.connected = false;
        state_.printerSessionResumePending = false;
        state_.printerSessionResumeSerial.clear();
        state_.printerSessionResumeProductId = 0;
        emit mediaListUpdated({});
        if (wasConnected) {
            emit deviceDisconnected();
        }
        const bool newPresence = isPrinterClassDevicePresent();
        if (oldPresence != newPresence) {
            emit printerPresenceChanged(newPresence);
        }
        return;
    }
    const bool ready =
        snapshot.state == PrinterProtocol::DiscoveryState::Ready &&
        snapshot.devices.size() == 1;
    const bool endpointSelected =
        ready && (state_.autoConnectMode || wasPrinterConnected);
    const QString snapshotSysfsPath = snapshot.devices.size() == 1
                                          ? snapshot.devices.first().sysfsPath
                                          : QString();
    const QString snapshotSerial =
        snapshot.devices.size() == 1 ? snapshot.devices.first().serial.trimmed()
                                     : QString();
    logPrinterLifecycleEvent(
        QStringLiteral("physical_generation_changed"), state_.printerGeneration,
        {{QStringLiteral("discovery_state"),
          printerDiscoveryStateName(snapshot.state)},
         {QStringLiteral("sysfs_path"), snapshotSysfsPath},
         {QStringLiteral("serial"), snapshotSerial},
         {QStringLiteral("endpoint_selected"),
          endpointSelected ? QStringLiteral("true") : QStringLiteral("false")},
         {QStringLiteral("disconnect_count"),
          QString::number(state_.printerDisconnectCount)},
         {QStringLiteral("lease_mode"),
          printerOverlayLeaseModeName(state_.printerOverlayLeaseMode)}});
    if (ready) {
        logPrinterLifecycleEvent(
            QStringLiteral("endpoint_discovered"), state_.printerGeneration,
            {{QStringLiteral("sysfs_path"), snapshotSysfsPath},
             {QStringLiteral("serial"), snapshotSerial},
             {QStringLiteral("endpoint_selected"),
              endpointSelected ? QStringLiteral("true")
                               : QStringLiteral("false")},
             {QStringLiteral("disconnect_count"),
              QString::number(state_.printerDisconnectCount)}});
    }
    const bool retryCacheValidationComplete =
        !callbacks_.retryCacheStartupSessionGateActive();
    const bool sessionLossAllowsSession =
        !state_.printerDisplaySessionLost ||
        state_.printerSessionLossRemovalObserved;
    const bool recoveryAllowsEndpoint =
        retryCacheValidationComplete && sessionLossAllowsSession &&
        (!state_.printerRecoveryRequired ||
         (endpointSelected && completePrinterRecoveryAfterRemoval(
                                  snapshot.devices.first().serial,
                                  snapshot.devices.first().productId)));
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    const bool restrictedRecoverySession =
        recoveryAllowsEndpoint &&
        callbacks_.retryCacheRestrictedRecoveryActive();
    const bool recoveryAllowsSession =
        recoveryAllowsEndpoint && !restrictedRecoverySession;
    const bool resumeSelectedSession =
        endpointSelected && state_.printerSessionResumePending &&
        !snapshot.devices.first().serial.isEmpty() &&
        snapshot.devices.first().serial == state_.printerSessionResumeSerial &&
        snapshot.devices.first().productId ==
            state_.printerSessionResumeProductId;
    if (ready && state_.printerSessionResumePending && !resumeSelectedSession) {
        state_.printerSessionResumePending = false;
        state_.printerSessionResumeSerial.clear();
        state_.printerSessionResumeProductId = 0;
    }
    emit requestGenerationGate(state_.printerGeneration,
                               endpointSelected && (recoveryAllowsSession ||
                                                    restrictedRecoverySession));
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }

    if (endpointSelected) {
        const QString newPath = snapshot.devices.first().devicePath;
        const QString newSerial = snapshot.devices.first().serial;
        const quint16 newProductId = snapshot.devices.first().productId;
        if (wasPrinterConnected &&
            (oldPath != newPath || oldSerial != newSerial ||
             oldProductId != newProductId)) {
            detachPrinterClassDevice(true);
        } else if (wasPrinterConnected) {
            emit printerDeviceVersionsReady(QString(), QString());
        }
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        emit requestConfigurePrinter(newPath, newSerial, newProductId,
                                     generation);
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        const std::optional<PrinterProductProfile> productProfile =
            printerProductProfileForId(newProductId);
        const PrinterProtocol::PaseOverlayConfig restoredOverlay =
            productProfile && productProfile->overlayMetricsSupported
                ? persistedPaseOverlayForDevice(newSerial)
                : PrinterProtocol::PaseOverlayConfig{};
        state_.metricsState.deviceSerial = newSerial.trimmed();
        state_.metricsState.enabled = paseOverlayHasMetrics(restoredOverlay);
        state_.metricsState.samplingActive = false;
        state_.metricsState.metrics = restoredOverlay.left.metrics;
        state_.metricsState.alignment = restoredOverlay.left.alignment;
        state_.metricsState.textColor = restoredOverlay.left.textColor;
        state_.metricsState.availableMetrics.clear();
        state_.metricsState.diagnostic.clear();
        publishMetricsState();
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        if (recoveryAllowsSession && productProfile &&
            productProfile->overlayMetricsSupported &&
            paseOverlayHasContent(restoredOverlay)) {
            emit requestRestorePrinterOverlay(restoredOverlay, generation);
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
        }
        if (resumeSelectedSession) {
            emit uploadStatus(tryx::DeviceManagerMessages::tr(
                "Restoring the active PASE display session after USB "
                "re-enumeration..."));
        }
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        if (recoveryAllowsSession) {
            state_.printerDisplaySessionLost = false;
            state_.printerSessionLossRemovalObserved = false;
            emit requestStartPrinterSession(newPath, generation);
        } else if (!retryCacheValidationComplete &&
                   !state_.printerDisplaySessionLost) {
            state_.printerDisplaySessionLost = false;
            emit uploadStatus(tryx::DeviceManagerMessages::tr(
                "Stored retry media is still being validated; the PASE display "
                "session will start only after validation finishes"));
        } else {
            state_.printerDisplaySessionLost = !restrictedRecoverySession;
            emit uploadStatus(printerMutationUnavailableStatusText());
        }
    } else {
        emit requestClearPrinter(generation);
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        if (wasPrinterConnected) {
            detachPrinterClassDevice(true);
        }
    }
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    if (wasPrinterConnected) {
        emit mediaListUpdated({});
    }
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }

    if (snapshot.blocksLegacyTransport() && state_.connected &&
        !state_.printerClassConnected) {
        state_.legacyProductId.clear();
        state_.connected = false;
        emit mediaListUpdated({});
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        emit requestDisconnect();
    }
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }

    const bool newPresence = isPrinterClassDevicePresent();
    if (oldPresence != newPresence) {
        emit printerPresenceChanged(newPresence);
    }

    if (!sessionTransitionIsCurrent(generation) || !state_.autoConnectMode) {
        return;
    }

    switch (snapshot.state) {
    case PrinterProtocol::DiscoveryState::Ready:
        if (!state_.printerClassConnected && snapshot.devices.size() == 1) {
            attachPrinterClassDevice(snapshot.devices.first());
        }
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
        callbacks_.startRetryCacheReadOnlyReconciliationIfReady();
        break;
    case PrinterProtocol::DiscoveryState::RockchipGadget391a0006:
    case PrinterProtocol::DiscoveryState::EnumeratingPrinterClass:
        emit uploadStatus(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::PermissionDenied:
    case PrinterProtocol::DiscoveryState::Ambiguous:
    case PrinterProtocol::DiscoveryState::MonitoringUnavailable:
        emit deviceError(snapshot.statusText());
        break;
    case PrinterProtocol::DiscoveryState::Absent:
        if (!state_.connected) {
            const auto legacyPort = panorama::Device::find_device();
            if (legacyPort) {
                emit requestConnect(QString::fromStdString(*legacyPort));
            } else {
                emit uploadStatus(tryx::DeviceManagerMessages::tr(
                    "Waiting for TRYX device. Reconnect USB or keep Auto "
                    "connection selected."));
            }
        }
        break;
    }
}

void PrinterSessionController::connectDevice(const QString &port) {
    if (callbacks_.runtimeDowngradePrepared()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    if (state_.firmwareRecoveryInterlockActive) {
        emit deviceError(tryx::DeviceManagerMessages::tr(
            "Device connection is blocked until firmware recovery is "
            "explicitly acknowledged"));
        return;
    }
    if (!port.isEmpty() && state_.printerSnapshot.blocksLegacyTransport()) {
        emit deviceError(tryx::DeviceManagerMessages::tr(
            "A TRYX printer-class or Rockchip gadget device is present; use "
            "Auto connection."));
        return;
    }
    const quint64 previousGeneration = state_.printerGeneration;
    setPrinterDisplaySessionActive(false);
    if (!sessionTransitionIsCurrent(previousGeneration)) {
        return;
    }
    clearDeviceSpecificationsCache();
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    state_.autoConnectMode = port.isEmpty();
    const bool wasLegacyConnected =
        state_.connected && !state_.printerClassConnected;
    state_.legacyProductId.clear();
    if (wasLegacyConnected) {
        state_.connected = false;
        emit mediaListUpdated({});
        if (!sessionTransitionIsCurrent(previousGeneration)) {
            return;
        }
        emit deviceDisconnected();
    }
    if (!sessionTransitionIsCurrent(previousGeneration)) {
        return;
    }

    if (port.isEmpty()) {
        if (state_.printerSnapshot.state ==
                PrinterProtocol::DiscoveryState::Ready &&
            state_.printerSnapshot.devices.size() == 1) {
            callbacks_.cancelForegroundForGenerationChange(
                tryx::DeviceManagerMessages::tr(
                    "Printer-class connection was restarted"));
            if (!sessionTransitionIsCurrent(previousGeneration)) {
                return;
            }
            const quint64 generation = ++state_.printerGeneration;
            invalidateDisplaySnapshot(true);
            state_.printerGenerationElapsedTimer.start();
            emit requestCancelPrinterPreparation(generation);
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            const bool retryCacheValidationComplete =
                !callbacks_.retryCacheStartupSessionGateActive();
            const bool sessionLossAllowsSession =
                !state_.printerDisplaySessionLost ||
                state_.printerSessionLossRemovalObserved;
            const bool recoveryAllowsEndpoint =
                retryCacheValidationComplete && sessionLossAllowsSession &&
                (!state_.printerRecoveryRequired ||
                 completePrinterRecoveryAfterRemoval(
                     state_.printerSnapshot.devices.first().serial,
                     state_.printerSnapshot.devices.first().productId));
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            const bool restrictedRecoverySession =
                recoveryAllowsEndpoint &&
                callbacks_.retryCacheRestrictedRecoveryActive();
            const bool recoveryAllowsSession =
                recoveryAllowsEndpoint && !restrictedRecoverySession;
            emit requestGenerationGate(
                generation, recoveryAllowsSession || restrictedRecoverySession);
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            emit requestConfigurePrinter(
                state_.printerSnapshot.devices.first().devicePath,
                state_.printerSnapshot.devices.first().serial,
                state_.printerSnapshot.devices.first().productId, generation);
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            const std::optional<PrinterProductProfile> productProfile =
                printerProductProfileForId(
                    state_.printerSnapshot.devices.first().productId);
            const PrinterProtocol::PaseOverlayConfig restoredOverlay =
                productProfile && productProfile->overlayMetricsSupported
                    ? persistedPaseOverlayForDevice(
                          state_.printerSnapshot.devices.first().serial)
                    : PrinterProtocol::PaseOverlayConfig{};
            state_.metricsState.deviceSerial =
                state_.printerSnapshot.devices.first().serial.trimmed();
            state_.metricsState.enabled =
                paseOverlayHasMetrics(restoredOverlay);
            state_.metricsState.samplingActive = false;
            state_.metricsState.metrics = restoredOverlay.left.metrics;
            state_.metricsState.alignment = restoredOverlay.left.alignment;
            state_.metricsState.textColor = restoredOverlay.left.textColor;
            state_.metricsState.availableMetrics.clear();
            state_.metricsState.diagnostic.clear();
            publishMetricsState();
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            if (recoveryAllowsSession && productProfile &&
                productProfile->overlayMetricsSupported &&
                paseOverlayHasContent(restoredOverlay)) {
                emit requestRestorePrinterOverlay(restoredOverlay, generation);
                if (!sessionTransitionIsCurrent(generation)) {
                    return;
                }
            }
            attachPrinterClassDevice(state_.printerSnapshot.devices.first());
            if (!sessionTransitionIsCurrent(generation)) {
                return;
            }
            if (recoveryAllowsSession) {
                state_.printerDisplaySessionLost = false;
                state_.printerSessionLossRemovalObserved = false;
                emit requestStartPrinterSession(
                    state_.printerSnapshot.devices.first().devicePath,
                    generation);
            } else if (!retryCacheValidationComplete &&
                       !state_.printerDisplaySessionLost) {
                state_.printerDisplaySessionLost = false;
                emit uploadStatus(tryx::DeviceManagerMessages::tr(
                    "Stored retry media is still being validated; the PASE "
                    "display session will start only after validation "
                    "finishes"));
            } else {
                state_.printerDisplaySessionLost = !restrictedRecoverySession;
                emit uploadStatus(printerMutationUnavailableStatusText());
            }
            if (sessionTransitionIsCurrent(generation)) {
                callbacks_.startRetryCacheReadOnlyReconciliationIfReady();
            }
            return;
        }
        if (state_.printerSnapshot.blocksLegacyTransport()) {
            state_.legacyProductId.clear();
            state_.connected = false;
            state_.printerClassConnected = false;
            state_.printerDevicePath.clear();
            const QString status = state_.printerSnapshot.statusText();
            if (state_.printerSnapshot.state ==
                    PrinterProtocol::DiscoveryState::PermissionDenied ||
                state_.printerSnapshot.state ==
                    PrinterProtocol::DiscoveryState::Ambiguous ||
                state_.printerSnapshot.state ==
                    PrinterProtocol::DiscoveryState::MonitoringUnavailable) {
                emit deviceError(status);
            } else {
                emit uploadStatus(status);
            }
            return;
        }

        const auto legacyPort = panorama::Device::find_device();
        if (!legacyPort) {
            state_.legacyProductId.clear();
            state_.connected = false;
            state_.printerClassConnected = false;
            state_.printerDevicePath.clear();
            emit uploadStatus(tryx::DeviceManagerMessages::tr(
                "Waiting for TRYX device. Reconnect USB or keep Auto "
                "connection selected."));
            return;
        }
        emit requestConnect(QString::fromStdString(*legacyPort));
        return;
    }

    detachPrinterClassDevice(false);
    if (sessionTransitionIsCurrent(previousGeneration)) {
        emit requestConnect(port);
    }
}

void PrinterSessionController::disconnectDevice() {
    if (callbacks_.runtimeDowngradePrepared()) {
        emit deviceError(printerMutationUnavailableStatusText());
        return;
    }
    if (firmwareExclusiveActive()) {
        emit deviceError(firmwareExclusiveStatusText());
        return;
    }
    // The recovery interlock forbids reconnect, but must still allow closing
    // the transport. Only a replacement generation or exclusive owner wins.
    const auto disconnectIsCurrent = [this](quint64 generation) {
        return generation == state_.printerGeneration &&
               !callbacks_.runtimeDowngradePrepared() &&
               !firmwareExclusiveActive();
    };
    const quint64 previousGeneration = state_.printerGeneration;
    state_.autoConnectMode = false;
    setPrinterDisplaySessionActive(false);
    if (!disconnectIsCurrent(previousGeneration)) {
        return;
    }
    clearDeviceSpecificationsCache();
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    stopKeepalive();
    const bool notifyPrinterDisconnect = state_.printerClassConnected;
    detachPrinterClassDevice(false);
    if (!disconnectIsCurrent(previousGeneration)) {
        return;
    }
    callbacks_.cancelForegroundForGenerationChange(
        tryx::DeviceManagerMessages::tr("Printer-class operation stopped "
                                        "because the device was disconnected"));
    if (!disconnectIsCurrent(previousGeneration)) {
        return;
    }
    const quint64 generation = ++state_.printerGeneration;
    invalidateDisplaySnapshot(true);
    emit requestCancelPrinterPreparation(generation);
    if (!disconnectIsCurrent(generation)) {
        return;
    }
    if (notifyPrinterDisconnect) {
        emit printerOperationsCancelled();
    }
    if (!disconnectIsCurrent(generation)) {
        return;
    }
    emit requestGenerationGate(generation, false);
    if (!disconnectIsCurrent(generation)) {
        return;
    }
    emit requestClearPrinter(generation);
    if (!disconnectIsCurrent(generation)) {
        return;
    }
    emit requestDisconnect();
    if (!disconnectIsCurrent(generation)) {
        return;
    }
    state_.connected = false;
    state_.legacyProductId.clear();
    emit mediaListUpdated({});
    if (notifyPrinterDisconnect && disconnectIsCurrent(generation)) {
        emit deviceDisconnected();
    }
}

void PrinterSessionController::requestDeviceInfo() {
    if (firmwareExclusiveActive()) {
        emit printerDeviceInfoFailed(firmwareExclusiveStatusText());
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (!devicePath.isEmpty()) {
        if (callbacks_.retryCacheMutationGateActive() ||
            state_.printerRecoveryRequired ||
            state_.printerDisplaySessionLost) {
            emit printerDeviceInfoFailed(
                printerMutationUnavailableStatusText());
            return;
        }
        emit requestPrinterDeviceInfo(devicePath, state_.printerGeneration);
        return;
    }
    if (state_.printerSnapshot.blocksLegacyTransport()) {
        emit printerDeviceInfoFailed(state_.printerSnapshot.statusText());
        return;
    }
    if (state_.connected) {
        emit uploadStatus(tryx::DeviceManagerMessages::tr(
            "Legacy device information is available from its connection "
            "handshake."));
        return;
    }
    emit printerDeviceInfoFailed(
        tryx::DeviceManagerMessages::tr("TRYX device is not connected"));
}

bool PrinterSessionController::isPrinterClassDevicePresent() const {
    return state_.printerClassConnected ||
           state_.printerSnapshot.blocksLegacyTransport();
}

void PrinterSessionController::attachPrinterClassDevice(
    const PrinterProtocol::UsbPrinterDevice &device) {
    if (state_.printerClassConnected &&
        state_.printerDevicePath == device.devicePath &&
        state_.printerProductId == device.productId) {
        return;
    }
    stopKeepalive();
    clearDeviceSpecificationsCache();
    state_.connected = true;
    state_.printerClassConnected = true;
    state_.legacyProductId.clear();
    const quint64 generation = state_.printerGeneration;
    setPrinterDisplaySessionActive(false);
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    state_.printerDevicePath = device.devicePath;
    state_.printerDeviceSerial = device.serial;
    state_.printerProductId = device.productId;
    invalidateDisplaySnapshot();
    callbacks_.clearMediaCatalogView();
    emit mediaListUpdated({});
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    emit deviceConnected(printerProductIdString(device.productId),
                         device.serial.isEmpty() ? device.devicePath
                                                 : device.serial,
                         QString(), QString());
}

void PrinterSessionController::detachPrinterClassDevice(bool notify) {
    const bool wasConnected = state_.printerClassConnected;
    const quint64 generation = state_.printerGeneration;
    clearDeviceSpecificationsCache();
    state_.printerClassConnected = false;
    state_.printerDevicePath.clear();
    state_.printerDeviceSerial.clear();
    state_.printerProductId = 0;
    invalidateDisplaySnapshot();
    if (wasConnected) {
        state_.displayStateReadGeneration = 0;
        const quint64 metricsRevision = state_.metricsState.revision;
        state_.metricsState = TryxRuntimeMetricsState{};
        state_.metricsState.revision = metricsRevision;
        publishMetricsState();
        if (generation != state_.printerGeneration) {
            return;
        }
        const quint64 displayRevision = state_.displayState.revision;
        state_.displayState = TryxRuntimeDisplayState{};
        state_.displayState.revision = displayRevision;
        publishDisplayState();
        if (generation != state_.printerGeneration) {
            return;
        }
        state_.connected = false;
        callbacks_.clearMediaCatalogView();
        emit mediaListUpdated({});
        if (notify && generation == state_.printerGeneration) {
            emit deviceDisconnected();
        }
    }
}

QString PrinterSessionController::currentPrinterPath() const {
    if (!state_.printerClassConnected ||
        state_.printerSnapshot.state !=
            PrinterProtocol::DiscoveryState::Ready ||
        state_.printerSnapshot.devices.size() != 1) {
        return {};
    }
    return state_.printerSnapshot.devices.first().devicePath;
}

std::optional<PrinterProductProfile> PrinterSessionController::
    currentPrinterProductProfile() const {
    quint16 productId = state_.printerProductId;
    if (productId == 0 &&
        state_.printerSnapshot.state ==
            PrinterProtocol::DiscoveryState::Ready &&
        state_.printerSnapshot.devices.size() == 1) {
        productId = state_.printerSnapshot.devices.first().productId;
    }
    return printerProductProfileForId(productId);
}

bool PrinterSessionController::currentPrinterSupportsMediaCatalog() const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->mediaCatalogSupported;
}

bool PrinterSessionController::currentPrinterSupportsDisplayConfiguration()
    const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->displayConfigurationSupported;
}

bool PrinterSessionController::currentPrinterSupportsOverlayMetrics() const {
    const auto profile = currentPrinterProductProfile();
    return profile && profile->overlayMetricsSupported;
}

bool PrinterSessionController::firmwareFlashAllowedForCurrentDevice(
    QString *errorMessage) const {
    const auto profile = currentPrinterProductProfile();
    if (profile && profile->firmwareFlashSupported) {
        return true;
    }
    const bool identifiedLegacyPanoramaSe =
        !profile && state_.connected && !state_.printerClassConnected &&
        !state_.printerSnapshot.blocksLegacyTransport() &&
        state_.legacyProductId.compare(QStringLiteral("cm01"),
                                       Qt::CaseInsensitive) == 0;
    if (identifiedLegacyPanoramaSe) {
        return true;
    }
    if (errorMessage) {
        *errorMessage =
            profile
                ? tryx::DeviceManagerMessages::tr(
                      "Firmware flashing is not supported for USB product %1")
                      .arg(printerProductIdString(profile->productId))
                : tryx::DeviceManagerMessages::tr(
                      "Firmware flashing requires a connected, identified "
                      "firmware-capable TRYX device");
    }
    return false;
}

QString PrinterSessionController::printerUnavailableStatusText() const {
    if (state_.printerSnapshot.state ==
            PrinterProtocol::DiscoveryState::Ready &&
        state_.printerSnapshot.devices.size() == 1 &&
        !state_.printerClassConnected) {
        return tryx::DeviceManagerMessages::tr(
            "TRYX endpoint is present, but the display session stopped. "
            "Reconnect USB or select Auto connection again.");
    }
    return state_.printerSnapshot.statusText();
}

QString PrinterSessionController::printerMutationUnavailableStatusText() const {
    if (callbacks_.runtimeDowngradePrepared()) {
        return tryx::DeviceManagerMessages::tr(
            "Device mutations are blocked because runtime downgrade "
            "preparation is committed");
    }
    if (firmwareExclusiveActive()) {
        return firmwareExclusiveStatusText();
    }
    if (callbacks_.retryCacheStoreBlocksMutations()) {
        return tryx::DeviceManagerMessages::tr(
            "Device mutations are blocked because the retry-cache transition "
            "is invalid or unsafe. Preserve the cache and inspect the runtime "
            "logs before retrying.");
    }
    if (callbacks_.retryCacheValidationPending()) {
        return tryx::DeviceManagerMessages::tr(
            "Stored retry media is still being validated; wait for validation "
            "to finish before using the PASE display session.");
    }
    if (callbacks_.retryCacheRestrictedRecoveryActive()) {
        return tryx::DeviceManagerMessages::tr(
            "Device mutations are blocked while the stored upload is resolved "
            "through read-only FileList reconciliation or a proven physical "
            "reconnect.");
    }
    if (state_.printerRecoveryRequired) {
        return tryx::DeviceManagerMessages::tr(
            "PASE must be power-cycled before another upload or display "
            "change. Disconnect its USB/power while the TRYX runtime is "
            "running, reconnect it, and wait for the display session to become "
            "active.");
    }
    if (state_.printerDisplaySessionLost) {
        return tryx::DeviceManagerMessages::tr(
            "The PASE display session is lost. Reconnect the device and wait "
            "for a new display session before trying again.");
    }
    if (!state_.printerDisplaySessionActive) {
        return tryx::DeviceManagerMessages::tr(
            "The PASE display session is not ready yet. Wait until the device "
            "finishes connecting before trying again.");
    }
    return printerUnavailableStatusText();
}

QString PrinterSessionController::firmwareExclusiveStatusText() const {
    return tryx::DeviceManagerMessages::tr(
        "Device controls are unavailable while firmware flashing owns the USB "
        "transport");
}

void PrinterSessionController::resumePrinterSessionAfterRetryCacheValidation() {
    if (!callbacks_.workerAvailable() || firmwareExclusiveActive() ||
        state_.firmwareRecoveryInterlockActive ||
        callbacks_.retryCacheMutationGateActive() ||
        state_.printerRecoveryRequired) {
        return;
    }
    if (state_.printerDisplaySessionLost &&
        !state_.printerSessionLossRemovalObserved) {
        emit uploadStatus(printerMutationUnavailableStatusText());
        return;
    }
    const QString devicePath = currentPrinterPath();
    if (devicePath.isEmpty()) {
        return;
    }

    const quint64 generation = state_.printerGeneration;
    emit requestGenerationGate(generation, true);
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    const PrinterProtocol::PaseOverlayConfig restoredOverlay =
        currentPrinterSupportsOverlayMetrics()
            ? persistedPaseOverlayForDevice(state_.printerDeviceSerial)
            : PrinterProtocol::PaseOverlayConfig{};
    if (currentPrinterSupportsOverlayMetrics() &&
        paseOverlayHasContent(restoredOverlay)) {
        emit requestRestorePrinterOverlay(restoredOverlay, generation);
        if (!sessionTransitionIsCurrent(generation)) {
            return;
        }
    }
    state_.printerDisplaySessionLost = false;
    state_.printerSessionLossRemovalObserved = false;
    emit requestStartPrinterSession(devicePath, generation);
}

void PrinterSessionController::requirePrinterRecovery(const QString &message) {
    const bool enteringRecovery = !state_.printerRecoveryRequired;
    state_.printerRecoveryRequired = true;
    if (enteringRecovery) {
        state_.printerRecoveryRemovalObserved =
            state_.printerSnapshot.state ==
            PrinterProtocol::DiscoveryState::Absent;
    }
    state_.printerDisplaySessionLost = true;
    clearDeviceSpecificationsCache();
    state_.printerSessionLossRemovalObserved = false;
    setPrinterDisplaySessionActive(false);
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    if (callbacks_.workerAvailable()) {
        if (enteringRecovery) {
            ++state_.printerGeneration;
            invalidateDisplaySnapshot(true);
            emit requestCancelPrinterPreparation(state_.printerGeneration);
        }
        emit requestGenerationGate(state_.printerGeneration, false);
        emit requestClearPrinter(state_.printerGeneration);
    }
    if (!message.isEmpty()) {
        emit uploadStatus(message);
    }
}

bool PrinterSessionController::completePrinterRecoveryAfterRemoval(
    const QString &currentDeviceIdentity, quint16 currentProductId) {
    if (!state_.printerRecoveryRequired) {
        return true;
    }
    if (!state_.printerRecoveryRemovalObserved) {
        return false;
    }
    const quint64 generation = state_.printerGeneration;
    if (!callbacks_.completeRetryRecoveryAfterRemoval(currentDeviceIdentity,
                                                      currentProductId) ||
        !sessionTransitionIsCurrent(generation)) {
        return false;
    }

    state_.printerRecoveryRequired = false;
    state_.printerRecoveryRemovalObserved = false;
    state_.printerDisplaySessionLost = false;
    state_.printerSessionLossRemovalObserved = false;
    emit uploadStatus(tryx::DeviceManagerMessages::tr(
        "PASE power-cycle was observed; starting a clean display session"));
    return sessionTransitionIsCurrent(generation);
}

bool PrinterSessionController::acquireFirmwareExclusive(const QString &leaseId,
                                                        QString *errorMessage) {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };
    if (QThread::currentThread() != thread()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "The firmware transport gate must be acquired on the runtime "
            "thread"));
    }
    if (!callbacks_.workerAvailable() || !callbacks_.workerRunning()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "The local device transport is unavailable for firmware flashing"));
    }
    QString productError;
    if (!firmwareFlashAllowedForCurrentDevice(&productError)) {
        return fail(productError);
    }
    const QString normalizedLease = leaseId.trimmed();
    if (normalizedLease.isEmpty()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "The firmware transport lease is invalid"));
    }
    if (firmwareExclusiveActive()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "Another firmware operation already owns the device transport"));
    }
    if (!callbacks_.activeOperationId().isEmpty()) {
        return fail(tryx::DeviceManagerMessages::tr(
                        "Device operation %1 is still active")
                        .arg(callbacks_.activeOperationId()));
    }
    if (callbacks_.retryCacheMutationGateActive()) {
        return fail(
            tryx::DeviceManagerMessages::tr("Stored retry media is still being "
                                            "validated or requires recovery"));
    }
    if (callbacks_.hasUnresolvedRetryOutcomeForFirmware()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "A previous media transfer has an unresolved device outcome; "
            "cancel or reconcile it before firmware flashing"));
    }
    if (callbacks_.hasPendingDeleteRecovery()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "A previous delete command still requires read-only "
            "reconciliation"));
    }
    if (callbacks_.hasPendingReplaceRecovery()) {
        return fail(tryx::DeviceManagerMessages::tr(
            "A previous replacement still requires read-only reconciliation"));
    }
    if (state_.printerRecoveryRequired) {
        return fail(tryx::DeviceManagerMessages::tr(
            "The PASE requires physical reconnect recovery before firmware "
            "flashing"));
    }
    if (state_.printerDisplaySessionLost) {
        return fail(tryx::DeviceManagerMessages::tr(
            "The PASE display session is lost; physically reconnect the device "
            "before firmware flashing"));
    }

    // All public device entry points run on this thread. Publishing the lease
    // before closing the worker generation gate makes the active-operation
    // check and mutation exclusion one indivisible event-loop transition.
    state_.firmwareExclusiveLeaseId = normalizedLease;
    state_.firmwareRecoveryReconnectRequested = false;
    state_.firmwareResumeAutoConnect = state_.autoConnectMode;
    clearDeviceSpecificationsCache();
    state_.firmwareQuiesceGeneration = ++state_.printerGeneration;
    invalidateDisplaySnapshot(true);
    firmwareQuiesceDispatchPending_ = true;
    setPrinterDisplaySessionActive(false);
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    stopKeepalive();
    emit requestCancelPrinterPreparation(state_.printerGeneration);
    emit requestGenerationGate(state_.printerGeneration, false);
    emit requestFirmwareTransportQuiesce(normalizedLease,
                                         state_.firmwareQuiesceGeneration);
    firmwareQuiesceDispatchPending_ = false;
    if (!state_.firmwareReleasePendingLeaseId.isEmpty()) {
        emit requestFirmwareQuiesceReleaseFence(
            state_.firmwareReleasePendingLeaseId,
            state_.firmwareQuiesceGeneration);
    }
    emit uploadStatus(tryx::DeviceManagerMessages::tr(
        "Device transport is reserved for firmware flashing"));
    return true;
}

void PrinterSessionController::releaseFirmwareExclusive(const QString &leaseId,
                                                        bool resumeTransport) {
    if (QThread::currentThread() != thread() || leaseId.trimmed().isEmpty() ||
        leaseId.trimmed() != state_.firmwareExclusiveLeaseId) {
        return;
    }
    if (!state_.firmwareReleasePendingLeaseId.isEmpty()) {
        if (state_.firmwareReleasePendingLeaseId == leaseId.trimmed()) {
            // A later shutdown request may downgrade an already queued resume.
            state_.firmwareReleaseResumeTransport =
                state_.firmwareReleaseResumeTransport && resumeTransport;
        }
        return;
    }
    state_.firmwareReleasePendingLeaseId = leaseId.trimmed();
    state_.firmwareReleaseResumeTransport =
        resumeTransport && state_.firmwareResumeAutoConnect;
    // A synchronous observer may cancel while acquire is still publishing
    // its state. Keep the lease and defer the fence until quiesce is queued.
    if (firmwareQuiesceDispatchPending_) {
        return;
    }
    emit requestFirmwareQuiesceReleaseFence(
        state_.firmwareReleasePendingLeaseId, state_.firmwareQuiesceGeneration);
}

void PrinterSessionController::setFirmwareRecoveryInterlockActive(bool active) {
    state_.firmwareRecoveryInterlockActive = active;
    if (active) {
        state_.autoConnectMode = false;
        stopKeepalive();
    }
}

void PrinterSessionController::
    resumeConnectionAfterFirmwareRecoveryAcknowledgement() {
    if (state_.firmwareRecoveryInterlockActive) {
        emit deviceError(tryx::DeviceManagerMessages::tr(
            "Device connection remains blocked by firmware recovery"));
        return;
    }
    if (firmwareExclusiveActive()) {
        // A firmware completion publishes its recovery state before the
        // worker-thread release fence necessarily returns. Preserve this
        // explicit user action and reconnect only after the old transport
        // queue is proven empty.
        state_.firmwareRecoveryReconnectRequested = true;
        return;
    }
    connectDevice();
}

void PrinterSessionController::publishMetricsState() {
    ++state_.metricsState.revision;
    emit metricsStateUpdated(state_.metricsState);
}

void PrinterSessionController::publishDisplayState() {
    ++state_.displayState.revision;
    emit displayStateUpdated(state_.displayState);
}

void PrinterSessionController::updateDisplayState(
    const PrinterProtocol::PaseDisplayState &state,
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    const int previousBrightness = state_.displayState.brightness;
    const bool hadValidState = state_.displayState.valid;
    const auto previousRevision = state_.displayState.revision;
    state_.displayState = projectDisplayState(state, overlay, state_.printerDeviceSerial.trimmed());
    state_.displayState.revision = previousRevision;
    publishDisplayState();
    if (!hadValidState ||
        previousBrightness != state_.displayState.brightness) {
        emit brightnessChanged(state_.displayState.brightness);
    }
}

TryxRuntimeDeviceCapabilitiesV1 PrinterSessionController::deviceCapabilitiesV1(
    quint64 connectionRevision) const {
    TryxRuntimeDeviceCapabilitiesV1 snapshot;
    snapshot.connectionRevision = connectionRevision;
    snapshot.physicalGeneration = state_.printerGeneration;
    if (!state_.connected || !state_.printerClassConnected) {
        return snapshot;
    }

    snapshot.deviceIdentity = state_.printerDeviceSerial.trimmed();
    const std::optional<PrinterProductProfile> profile =
        currentPrinterProductProfile();
    if (snapshot.deviceIdentity.isEmpty() || !profile) {
        snapshot.deviceIdentity.clear();
        return snapshot;
    }

    if (profile->mediaUploadSupported) {
        snapshot.capabilities.append(tryxDeviceMediaUploadV1Token());
    }
    if (profile->mediaCatalogSupported) {
        snapshot.capabilities.append(tryxDeviceMediaCatalogV1Token());
    }
    if (profile->displayConfigurationSupported) {
        snapshot.capabilities.append(tryxDeviceDisplayConfigurationV1Token());
    }
    if (profile->splitAreaMediaSupported) {
        snapshot.capabilities.append(tryxDeviceMediaSplitAreaV1Token());
    }
    if (profile->overlayMetricsSupported) {
        snapshot.capabilities.append(tryxDeviceOverlayMetricsV1Token());
    }
    if (profile->productId == 0x1021 && profile->overlayMetricsSupported && profile->displayConfigurationSupported)
        snapshot.capabilities.append(tryxDeviceOverlayBadgeTextV1Token());
    if (profile->firmwareFlashSupported) {
        snapshot.capabilities.append(tryxDeviceFirmwareFlashV1Token());
    }
    return snapshot;
}

TryxRuntimeDeviceSpecificationsV1 PrinterSessionController::
    deviceSpecificationsV1(const TryxRuntimeSnapshot &connection) const {
    TryxRuntimeDeviceSpecificationsV1 snapshot;
    snapshot.connectionRevision = connection.revision;
    if (!connection.connected) {
        return snapshot;
    }

    snapshot.deviceIdentity = connection.serial.trimmed();
    if (!connection.printerClassConnected) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }

    const QString currentIdentity = state_.printerDeviceSerial.trimmed();
    const bool exactConnectionContext =
        state_.connected && state_.printerClassConnected &&
        state_.printerGeneration != 0 && !currentIdentity.isEmpty() &&
        snapshot.deviceIdentity == currentIdentity &&
        connection.productId == printerProductIdString(state_.printerProductId);
    if (!exactConnectionContext) {
        snapshot.deviceIdentity.clear();
        snapshot.physicalGeneration = 0;
        return snapshot;
    }

    snapshot.physicalGeneration = state_.printerGeneration;
    if (state_.printerProductId != 0x1011 &&
        state_.printerProductId != 0x1021) {
        snapshot.status = QStringLiteral("Unsupported");
        return snapshot;
    }

    snapshot.status = QStringLiteral("Unavailable");
    const bool exactCache =
        state_.deviceSpecificationsCache.valid &&
        state_.deviceSpecificationsDevicePath == state_.printerDevicePath &&
        state_.deviceSpecificationsDeviceIdentity == currentIdentity &&
        state_.deviceSpecificationsProductId == state_.printerProductId &&
        state_.deviceSpecificationsGeneration == state_.printerGeneration;
    if (!exactCache) {
        return snapshot;
    }

    snapshot.status = QStringLiteral("Ready");
    snapshot.reportedProductName =
        state_.deviceSpecificationsCache.reportedProductName;
    snapshot.videoOutputWidth =
        state_.deviceSpecificationsCache.videoOutputWidth;
    snapshot.videoOutputHeight =
        state_.deviceSpecificationsCache.videoOutputHeight;
    snapshot.screenType = state_.deviceSpecificationsCache.screenType;
    snapshot.usbAutoKeepalive =
        state_.deviceSpecificationsCache.usbAutoKeepalive;
    return snapshot;
}

void PrinterSessionController::loadPaseMetricsConfig() {
    const auto result = paseMetricsConfigStore_->load();
    for (const QString &warning : result.warnings) {
        qWarning().noquote() << warning;
    }
}

bool PrinterSessionController::overlayConfigurationSupportsDowngradeV10() const {
    return paseMetricsConfigStore_ && paseMetricsConfigStore_->writesEnabled()
        && tryx::configurationVersionIsSupported(paseMetricsConfigStore_->configPath(), 64 * 1024, {1, 2});
}

bool PrinterSessionController::persistPaseMetricsConfiguration(
    const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
    QString *errorMessage) {
    const QString serial = state_.printerDeviceSerial.trimmed();
    if (serial.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Cannot persist PASE metrics without a device serial");
        }
        return false;
    }
    const auto result = enabled
                            ? paseMetricsConfigStore_->persist(serial, overlay)
                            : paseMetricsConfigStore_->clearForDevice(serial);
    if (!result.ok() && errorMessage) {
        *errorMessage = result.detail;
    }
    return result.ok();
}

PrinterProtocol::PaseOverlayConfig PrinterSessionController::
    persistedPaseOverlayForDevice(const QString &deviceSerial) const {
    const auto overlay =
        paseMetricsConfigStore_->overlayForDevice(deviceSerial);
    return overlay.value_or(PrinterProtocol::PaseOverlayConfig{});
}

void PrinterSessionController::promoteRestrictedSessionAfterProof() {
    const quint64 generation = state_.printerGeneration;
    if (!sessionTransitionIsCurrent(generation) ||
        callbacks_.retryCacheMutationGateActive() ||
        currentPrinterPath().isEmpty() || state_.printerRecoveryRequired) {
        return;
    }
    state_.printerDisplaySessionLost = false;
    state_.printerSessionLossRemovalObserved = false;
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    setPrinterDisplaySessionActive(true);
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    const bool samplingActive = currentPrinterSupportsOverlayMetrics() &&
                                state_.metricsState.enabled &&
                                !state_.metricsState.metrics.isEmpty();
    if (state_.metricsState.samplingActive != samplingActive) {
        state_.metricsState.samplingActive = samplingActive;
        publishMetricsState();
    }
    if (currentPrinterSupportsMediaCatalog()) {
        if (!printerResultIsCurrent(generation)) {
            return;
        }
        callbacks_.resumePendingDeleteReconciliation();
        if (printerResultIsCurrent(generation)) {
            callbacks_.resumePendingReplaceReconciliation();
        }
    }
}

void PrinterSessionController::startKeepalive(int intervalSec) {
    if (callbacks_.runtimeDowngradePrepared() || firmwareExclusiveActive()) {
        return;
    }
    if (state_.printerClassConnected) {
        keepaliveTimer_->stop();
        return;
    }
    keepaliveTimer_->start(qMax(1, intervalSec) * 1000);
}

void PrinterSessionController::stopKeepalive() {
    keepaliveTimer_->stop();
}

void PrinterSessionController::handleWorkerConnected(const QString &pid,
                                                     const QString &serial,
                                                     const QString &fw,
                                                     const QString &app) {
    if (callbacks_.runtimeDowngradePrepared() || firmwareExclusiveActive() ||
        state_.firmwareRecoveryInterlockActive) {
        return;
    }
    if (state_.printerSnapshot.blocksLegacyTransport()) {
        emit requestDisconnect();
        return;
    }
    state_.legacyProductId = pid.trimmed();
    state_.connected = true;
    state_.printerClassConnected = false;
    emit deviceConnected(pid, serial, fw, app);
}

void PrinterSessionController::handleWorkerDisconnected() {
    if (callbacks_.runtimeDowngradePrepared() || state_.printerClassConnected ||
        firmwareExclusiveActive()) {
        return;
    }
    state_.legacyProductId.clear();
    state_.connected = false;
    emit deviceDisconnected();
}

void PrinterSessionController::handleWorkerError(const QString &message) {
    if (!callbacks_.runtimeDowngradePrepared() && !firmwareExclusiveActive() &&
        !state_.firmwareRecoveryInterlockActive &&
        !state_.printerSnapshot.blocksLegacyTransport()) {
        emit deviceError(message);
    }
}

void PrinterSessionController::handleWorkerBrightnessSet(int value) {
    if (legacyResultIsCurrent()) {
        emit brightnessChanged(value);
    }
}

void PrinterSessionController::handleWorkerScreenConfigSet() {
    if (legacyResultIsCurrent()) {
        emit screenConfigChanged();
    }
}

void PrinterSessionController::handleWorkerMediaUploaded(
    const QString &fileName) {
    if (legacyResultIsCurrent()) {
        emit mediaUploaded(fileName);
    }
}

void PrinterSessionController::handleWorkerMediaDeleted() {
    if (legacyResultIsCurrent()) {
        emit mediaDeleted();
    }
}

void PrinterSessionController::handleWorkerMediaListReady(
    const QStringList &files) {
    if (legacyResultIsCurrent()) {
        emit mediaListUpdated(files);
    }
}

void PrinterSessionController::handleWorkerUploadProgress(
    const QString &status) {
    if (legacyResultIsCurrent()) {
        emit uploadStatus(status);
    }
}

void PrinterSessionController::handleWorkerPrinterOperationError(
    const QString &message, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit deviceError(message);
    }
}

void PrinterSessionController::handleWorkerPrinterUploadProgress(
    const QString &status, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit uploadStatus(status);
    }
}

void PrinterSessionController::handleWorkerPrinterSessionStarted(
    quint64 generation) {
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    if (callbacks_.retryCacheRestrictedRecoveryActive()) {
        setPrinterDisplaySessionActive(false);
        if (!printerResultIsCurrent(generation)) {
            return;
        }
        callbacks_.startRetryCacheReadOnlyReconciliationIfReady();
        emit uploadStatus(printerMutationUnavailableStatusText());
        return;
    }
    if (state_.printerRecoveryRequired ||
        callbacks_.retryCacheStartupSessionGateActive()) {
        setPrinterDisplaySessionActive(false);
        emit uploadStatus(printerMutationUnavailableStatusText());
        return;
    }
    const QString sysfsPath =
        state_.printerSnapshot.devices.size() == 1
            ? state_.printerSnapshot.devices.first().sysfsPath
            : QString();
    logPrinterLifecycleEvent(
        QStringLiteral("recovery_completed"), generation,
        {{QStringLiteral("sysfs_path"), sysfsPath},
         {QStringLiteral("serial"), state_.printerDeviceSerial.trimmed()},
         {QStringLiteral("disconnect_count"),
          QString::number(state_.printerDisconnectCount)},
         {QStringLiteral("elapsed_ms"),
          state_.printerGenerationElapsedTimer.isValid()
              ? QString::number(state_.printerGenerationElapsedTimer.elapsed())
              : QStringLiteral("-1")},
         {QStringLiteral("lease_mode"),
          printerOverlayLeaseModeName(state_.printerOverlayLeaseMode)}});
    state_.printerDisplaySessionLost = false;
    state_.printerSessionLossRemovalObserved = false;
    setPrinterDisplaySessionActive(true);
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    if (currentPrinterSupportsDisplayConfiguration() &&
        (!state_.displayState.valid ||
         state_.displayState.deviceSerial !=
             state_.printerDeviceSerial.trimmed()) &&
        state_.displayStateReadGeneration != generation) {
        state_.displayStateReadGeneration = generation;
        emit requestPrinterDisplayState(currentPrinterPath(), generation);
    }

    if (!printerResultIsCurrent(generation) || !callbacks_.sessionStarted() ||
        !printerResultIsCurrent(generation)) {
        return;
    }
    const bool samplingActive = currentPrinterSupportsOverlayMetrics() &&
                                state_.metricsState.enabled &&
                                !state_.metricsState.metrics.isEmpty();
    if (state_.metricsState.samplingActive != samplingActive) {
        state_.metricsState.samplingActive = samplingActive;
        publishMetricsState();
    }
    if (currentPrinterSupportsMediaCatalog()) {
        if (!printerResultIsCurrent(generation)) {
            return;
        }
        callbacks_.resumePendingDeleteReconciliation();
        if (printerResultIsCurrent(generation)) {
            callbacks_.resumePendingReplaceReconciliation();
        }
    }
}

void PrinterSessionController::handleWorkerPrinterSessionStopped(
    quint64 generation) {
    if (generation == state_.printerGeneration) {
        setPrinterDisplaySessionActive(false);
        if (generation != state_.printerGeneration) {
            return;
        }
        state_.printerSessionResumePending = false;
        state_.printerSessionResumeSerial.clear();
        state_.printerSessionResumeProductId = 0;
        if (state_.metricsState.samplingActive) {
            state_.metricsState.samplingActive = false;
            publishMetricsState();
        }
    }
}

void PrinterSessionController::handleWorkerFirmwareTransportQuiesced(
    const QString &leaseId, quint64 generation) {
    if (leaseId != state_.firmwareExclusiveLeaseId ||
        generation != state_.firmwareQuiesceGeneration) {
        return;
    }
    const bool wasConnected = state_.connected;
    detachPrinterClassDevice(false);
    state_.legacyProductId.clear();
    state_.connected = false;
    setPrinterDisplaySessionActive(false);
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    emit mediaListUpdated({});
    if (wasConnected) {
        emit deviceDisconnected();
    }
    emit firmwareTransportQuiesced(
        leaseId, true,
        tryx::DeviceManagerMessages::tr(
            "Device transports are closed for firmware flashing"));
}

void PrinterSessionController::handleWorkerFirmwareQuiesceReleaseFenceReached(
    const QString &leaseId, quint64 generation) {
    if (leaseId != state_.firmwareExclusiveLeaseId ||
        leaseId != state_.firmwareReleasePendingLeaseId ||
        generation != state_.firmwareQuiesceGeneration) {
        return;
    }
    const bool reconnect = state_.firmwareReleaseResumeTransport ||
                           state_.firmwareRecoveryReconnectRequested;
    state_.firmwareExclusiveLeaseId.clear();
    state_.firmwareReleasePendingLeaseId.clear();
    state_.firmwareQuiesceGeneration = 0;
    state_.firmwareResumeAutoConnect = false;
    state_.firmwareReleaseResumeTransport = false;
    state_.firmwareRecoveryReconnectRequested = false;
    if (reconnect) {
        connectDevice();
    } else {
        // A non-resuming release is a fail-closed recovery boundary,
        // not merely "do not reconnect right now". Disable passive
        // monitor-triggered reconnects until the user explicitly
        // starts a new connection after inspecting the device.
        state_.autoConnectMode = false;
    }
}

void PrinterSessionController::handleWorkerPrinterSessionLost(
    quint64 generation) {
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    clearDeviceSpecificationsCache();
    if (!callbacks_.sessionLostBeforeStateChange() ||
        !printerResultIsCurrent(generation)) {
        return;
    }
    if (!state_.printerDisplaySessionLost) {
        state_.printerSessionLossRemovalObserved = false;
    }
    state_.printerDisplaySessionLost = true;
    setPrinterDisplaySessionActive(false);
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    state_.printerSessionResumePending = false;
    state_.printerSessionResumeSerial.clear();
    state_.printerSessionResumeProductId = 0;
    if (state_.metricsState.samplingActive) {
        state_.metricsState.samplingActive = false;
        publishMetricsState();
    }
    callbacks_.cancelForegroundForGenerationChange(
        tryx::DeviceManagerMessages::tr("PASE display session was lost"));
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    emit printerOperationsCancelled();
    emit uploadStatus(
        tryx::DeviceManagerMessages::tr("PASE display session is lost; waiting "
                                        "for a new USB endpoint generation"));
}

void PrinterSessionController::handleWorkerPrinterMetricsAvailabilityChanged(
    const QStringList &availableMetrics, quint64 generation) {
    if (!printerResultIsCurrent(generation) ||
        state_.metricsState.availableMetrics == availableMetrics) {
        return;
    }
    state_.metricsState.availableMetrics = availableMetrics;
    publishMetricsState();
}

void PrinterSessionController::handleWorkerPrinterScreenConfigSet(
    quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit screenConfigChanged();
    }
}

void PrinterSessionController::handleWorkerPrinterDeviceVersionsReady(
    const QString &firmware, const QString &appVersion, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit printerDeviceVersionsReady(firmware, appVersion);
    }
}

void PrinterSessionController::handleWorkerPrinterDeviceSpecificationsReady(
    const PrinterProtocol::DeviceSpecifications &specifications,
    const QString &devicePath, const QString &deviceSerial, quint16 productId,
    quint64 generation) {
    const QString identity = deviceSerial.trimmed();
    const bool exactDiscoveryContext =
        state_.printerSnapshot.devices.size() == 1 &&
        state_.printerSnapshot.devices.first().devicePath == devicePath &&
        state_.printerSnapshot.devices.first().serial.trimmed() == identity &&
        state_.printerSnapshot.devices.first().productId == productId;
    if (!printerResultIsCurrent(generation) || generation == 0 ||
        devicePath != state_.printerDevicePath || identity.isEmpty() ||
        identity != state_.printerDeviceSerial.trimmed() ||
        productId != state_.printerProductId ||
        (productId != 0x1011 && productId != 0x1021) ||
        !exactDiscoveryContext) {
        return;
    }

    clearDeviceSpecificationsCache();
    const bool complete = specifications.valid &&
                          !specifications.reportedProductName.isEmpty() &&
                          specifications.reportedProductName.size() <= 128 &&
                          specifications.videoOutputWidth >= 1 &&
                          specifications.videoOutputWidth <= 16384 &&
                          specifications.videoOutputHeight >= 1 &&
                          specifications.videoOutputHeight <= 16384 &&
                          (specifications.screenType == QStringLiteral("LCD") ||
                           specifications.screenType == QStringLiteral("OLED"));
    if (!complete) {
        return;
    }

    state_.deviceSpecificationsCache = specifications;
    state_.deviceSpecificationsDevicePath = devicePath;
    state_.deviceSpecificationsDeviceIdentity = identity;
    state_.deviceSpecificationsProductId = productId;
    state_.deviceSpecificationsGeneration = generation;
}

void PrinterSessionController::handleWorkerPrinterDeviceInfoReady(
    const PrinterProtocol::DeviceInfo &info, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit printerDeviceVersionsReady(info.firmwareVersion, info.appVersion);
        if (printerResultIsCurrent(generation)) {
            emit printerDeviceInfoReady(info);
        }
    }
}

void PrinterSessionController::handleWorkerPrinterDeviceInfoFailed(
    const QString &message, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit printerDeviceInfoFailed(message);
    }
}

void PrinterSessionController::handleWorkerPrinterDisplayStateReady(
    const PrinterProtocol::PaseDisplayState &state, quint64 generation) {
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    const auto identity = state_.printerDeviceSerial.trimmed();
    if (displaySnapshot_.physicalGeneration != generation) invalidateDisplaySnapshot(true);
    updateDisplayState(
        state, persistedPaseOverlayForDevice(state_.printerDeviceSerial));
    if (!printerResultIsCurrent(generation) || identity != state_.printerDeviceSerial.trimmed()
        || !bootstrapDisplayAllowed_) return;
    if (displaySnapshotIdentity_ != identity) {
        invalidateDisplaySnapshot();
        if (!printerResultIsCurrent(generation) || identity != state_.printerDeviceSerial.trimmed()) return;
    }
    bootstrapDisplay_ = state;
    tryAcceptBootstrapDisplay();
}

void PrinterSessionController::handleWorkerPrinterDisplayStateFailed(
    const QString &message, quint64 generation) {
    if (!printerResultIsCurrent(generation)) {
        return;
    }
    state_.displayState.deviceSerial = state_.printerDeviceSerial.trimmed();
    state_.displayState.valid = false;
    state_.displayState.diagnostic =
        tryx::DeviceManagerMessages::tr("Failed to read PASE display state: %1")
            .arg(message);
    publishDisplayState();
}

void PrinterSessionController::handleWorkerSysinfoSent() {
    if (legacyResultIsCurrent()) {
        emit sysinfoSent();
    }
}

void PrinterSessionController::handleWorkerPrinterSysinfoSent(
    quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit sysinfoSent();
    }
}

void PrinterSessionController::handleWorkerPrinterSysinfoFailed(
    const QString &message, quint64 generation) {
    if (printerResultIsCurrent(generation)) {
        emit deviceError(
            tryx::DeviceManagerMessages::tr("Failed to update PASE metrics: %1")
                .arg(message));
    }
}

void PrinterSessionController::handleWorkerPrinterTransportReady(
    quint64 generation) {
    if (printerResultIsCurrent(generation) &&
        state_.printerDisplaySessionActive && isPrinterClassDevicePresent() &&
        callbacks_.activeOperationId().isEmpty()) {
        emit printerTransportReady();
    }
}

bool PrinterSessionController::legacyResultIsCurrent() const {
    return !callbacks_.runtimeDowngradePrepared() &&
           !firmwareExclusiveActive() &&
           !state_.firmwareRecoveryInterlockActive && state_.connected &&
           !state_.printerClassConnected &&
           !state_.printerSnapshot.blocksLegacyTransport();
}

bool PrinterSessionController::printerResultIsCurrent(
    quint64 generation) const {
    return sessionTransitionIsCurrent(generation) &&
           state_.printerClassConnected &&
           state_.printerSnapshot.state ==
               PrinterProtocol::DiscoveryState::Ready;
}

bool PrinterSessionController::sessionTransitionIsCurrent(
    quint64 generation) const {
    return !callbacks_.runtimeDowngradePrepared() &&
           !firmwareExclusiveActive() &&
           !state_.firmwareRecoveryInterlockActive &&
           generation == state_.printerGeneration;
}

void PrinterSessionController::publishOperationMetrics(
    const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
    const QString &diagnostic, bool updateDisplay) {
    state_.metricsState.deviceSerial = state_.printerDeviceSerial.trimmed();
    state_.metricsState.enabled = enabled;
    state_.metricsState.samplingActive = enabled;
    state_.metricsState.metrics = overlay.left.metrics;
    state_.metricsState.alignment = overlay.left.alignment;
    state_.metricsState.textColor = overlay.left.textColor;
    state_.metricsState.diagnostic = diagnostic;
    publishMetricsState();
    if (!updateDisplay || !state_.displayState.valid) {
        return;
    }
    PrinterProtocol::PaseDisplayState state;
    state.backlightEnabled = state_.displayState.backlightEnabled;
    state.brightness = state_.displayState.brightness;
    state.standbyEnabled = state_.displayState.standbyEnabled;
    state.standbyMedia = state_.displayState.standbyMedia;
    state.mirrorMode = state_.displayState.mirrorMode;
    state.waterfallMode = state_.displayState.waterfallMode;
    state.screenMode = state_.displayState.screenMode;
    state.playMode = state_.displayState.playMode;
    state.media = state_.displayState.media;
    updateDisplayState(state, overlay);
}

void PrinterSessionController::handleCurrentEndpointRemoved() {
    ++state_.printerDisconnectCount;
    const QString sysfsPath =
        state_.printerSnapshot.devices.size() == 1
            ? state_.printerSnapshot.devices.first().sysfsPath
            : QString();
    const QString serial =
        state_.printerSnapshot.devices.size() == 1
            ? state_.printerSnapshot.devices.first().serial.trimmed()
            : state_.printerDeviceSerial.trimmed();
    logPrinterLifecycleEvent(
        QStringLiteral("endpoint_removed"), state_.printerGeneration,
        {{QStringLiteral("sysfs_path"), sysfsPath},
         {QStringLiteral("serial"), serial},
         {QStringLiteral("disconnect_count"),
          QString::number(state_.printerDisconnectCount)},
         {QStringLiteral("lease_mode"),
          printerOverlayLeaseModeName(state_.printerOverlayLeaseMode)}});
    if (state_.printerRecoveryRequired && isPrinterClassDevicePresent()) {
        state_.printerRecoveryRemovalObserved = true;
    }
    if (state_.printerDisplaySessionLost && isPrinterClassDevicePresent()) {
        state_.printerSessionLossRemovalObserved = true;
    }
}

void PrinterSessionController::prepareRestrictedReadOnlySession(
    quint64 generation) {
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    setPrinterDisplaySessionActive(false);
    if (!sessionTransitionIsCurrent(generation)) {
        return;
    }
    stopKeepalive();
    if (callbacks_.workerAvailable()) {
        emit requestGenerationGate(generation, true);
    }
}

void PrinterSessionController::shutdownBeforeWorkersStopped() {
    stopKeepalive();
    callbacks_.cancelForegroundForGenerationChange(
        tryx::DeviceManagerMessages::tr("TRYX runtime is stopping"));
    clearDeviceSpecificationsCache();
    ++state_.printerGeneration;
    invalidateDisplaySnapshot(true);
    emit requestCancelPrinterPreparation(state_.printerGeneration);
    emit requestGenerationGate(state_.printerGeneration, false);
    emit requestClearPrinter(state_.printerGeneration);
    emit requestDisconnect();
}

void PrinterSessionController::invalidateForRuntimeDowngrade() {
    ++state_.printerGeneration;
    invalidateDisplaySnapshot(true);
    emit requestCancelPrinterPreparation(state_.printerGeneration);
    emit requestGenerationGate(state_.printerGeneration, false);
}
