#pragma once

#include "mediacatalogmodel.h"
#include "operationlistmodel.h"
#include "runtimecontract.h"
#include "savedlayoutlistmodel.h"

#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class RuntimeClient final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_PROPERTY(bool serviceAvailable READ serviceAvailable
                   NOTIFY connectionChanged)
    Q_PROPERTY(bool compatible READ compatible NOTIFY connectionChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY connectionChanged)
    Q_PROPERTY(bool legacyConnected READ legacyConnected
                   NOTIFY connectionChanged)
    Q_PROPERTY(bool printerClassDevicePresent
                   READ printerClassDevicePresent NOTIFY connectionChanged)
    Q_PROPERTY(bool displaySessionActive READ displaySessionActive
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString productId READ productId NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceModel READ deviceModel NOTIFY connectionChanged)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceAppVersion READ deviceAppVersion
                   NOTIFY connectionChanged)
    Q_PROPERTY(int mediaTargetWidth READ mediaTargetWidth
                   NOTIFY connectionChanged)
    Q_PROPERTY(int mediaTargetHeight READ mediaTargetHeight
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY diagnosticChanged)
    Q_PROPERTY(quint32 apiVersion READ apiVersion NOTIFY connectionChanged)
    Q_PROPERTY(quint32 expectedApiVersion READ expectedApiVersion CONSTANT)
    Q_PROPERTY(bool capabilitiesReady READ capabilitiesReady
                   NOTIFY capabilitiesChanged)
    Q_PROPERTY(QStringList runtimeCapabilities READ runtimeCapabilities
                   NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool supportSnapshotAvailable READ supportSnapshotAvailable
                   NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool supportSnapshotBusy READ supportSnapshotBusy
                   NOTIFY supportSnapshotStateChanged)
    Q_PROPERTY(bool deviceCapabilitiesReady READ deviceCapabilitiesReady
                   NOTIFY capabilitiesChanged)
    Q_PROPERTY(QStringList deviceCapabilities READ deviceCapabilities
                   NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool customBadgeTextSupported READ customBadgeTextSupported NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantMap displayBadgeChoices READ displayBadgeChoices NOTIFY displayChanged)
    Q_PROPERTY(bool deviceSpecificationsSupported
                   READ deviceSpecificationsSupported
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(QString deviceSpecificationsStatus
                   READ deviceSpecificationsStatus
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(bool deviceSpecificationsReady
                   READ deviceSpecificationsReady
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(QString deviceReportedProductName
                   READ deviceReportedProductName
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(int deviceVideoOutputWidth
                   READ deviceVideoOutputWidth
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(int deviceVideoOutputHeight
                   READ deviceVideoOutputHeight
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(QString deviceScreenType READ deviceScreenType
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(bool deviceUsbAutoKeepalive
                   READ deviceUsbAutoKeepalive
                   NOTIFY deviceSpecificationsChanged)
    Q_PROPERTY(bool presentationPreferencesReady
                   READ presentationPreferencesReady
                   NOTIFY presentationPreferencesChanged)
    Q_PROPERTY(bool presentationPreferencesBusy
                   READ presentationPreferencesBusy
                   NOTIFY presentationPreferencesChanged)
    Q_PROPERTY(QString temperatureUnit READ temperatureUnit
                   NOTIFY presentationPreferencesChanged)
    Q_PROPERTY(QString timeFormat READ timeFormat
                   NOTIFY presentationPreferencesChanged)
    Q_PROPERTY(SavedLayoutListModel *savedLayoutModel
                   READ savedLayoutModel CONSTANT)
    Q_PROPERTY(bool savedLayoutsSupported READ savedLayoutsSupported
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(bool savedLayoutsReady READ savedLayoutsReady
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(bool savedLayoutsBusy READ savedLayoutsBusy
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(QString savedLayoutsStatus READ savedLayoutsStatus
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(QString savedLayoutsDiagnostic READ savedLayoutsDiagnostic
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(QString savedLayoutsDeviceIdentity
                   READ savedLayoutsDeviceIdentity
                   NOTIFY savedLayoutsChanged)
    Q_PROPERTY(MediaCatalogModel *mediaModel READ mediaModel CONSTANT)
    Q_PROPERTY(OperationListModel *operationModel READ operationModel CONSTANT)
    Q_PROPERTY(bool operationBusy READ operationBusy
                   NOTIFY operationChanged)
    Q_PROPERTY(QString activeOperationId READ activeOperationId
                   NOTIFY operationChanged)
    Q_PROPERTY(QString operationSummary READ operationSummary
                   NOTIFY operationChanged)
    Q_PROPERTY(double operationProgress READ operationProgress
                   NOTIFY operationChanged)
    Q_PROPERTY(QStringList availableMetrics READ availableMetrics
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool metricsCatalogReady READ metricsCatalogReady
                   NOTIFY metricsCatalogChanged)
    Q_PROPERTY(QStringList metricsCatalog READ metricsCatalog
                   NOTIFY metricsCatalogChanged)
    Q_PROPERTY(bool metricsEnabled READ metricsEnabled
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool samplingActive READ samplingActive
                   NOTIFY metricsChanged)
    Q_PROPERTY(QStringList activeMetrics READ activeMetrics
                   NOTIFY metricsChanged)
    Q_PROPERTY(QString metricsAlignment READ metricsAlignment
                   NOTIFY metricsChanged)
    Q_PROPERTY(QString metricsColor READ metricsColor
                   NOTIFY metricsChanged)
    Q_PROPERTY(bool displayStateValid READ displayStateValid
                   NOTIFY displayChanged)
    Q_PROPERTY(bool legacyDisplayLayoutConfirmed
                   READ legacyDisplayLayoutConfirmed
                   NOTIFY displayChanged)
    Q_PROPERTY(bool legacyDisplayBrightnessConfirmed
                   READ legacyDisplayBrightnessConfirmed
                   NOTIFY displayChanged)
    Q_PROPERTY(quint64 displayRevision READ displayRevision
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayDeviceIdentity
                   READ displayDeviceIdentity
                   NOTIFY displayDeviceIdentityChanged)
    Q_PROPERTY(int brightness READ brightness NOTIFY displayChanged)
    Q_PROPERTY(bool backlightEnabled READ backlightEnabled
                   NOTIFY displayChanged)
    Q_PROPERTY(bool mirrorMode READ mirrorMode NOTIFY displayChanged)
    Q_PROPERTY(bool waterfallMode READ waterfallMode NOTIFY displayChanged)
    Q_PROPERTY(QString currentScreenMode READ currentScreenMode
                   NOTIFY displayChanged)
    Q_PROPERTY(QString currentPlayMode READ currentPlayMode
                   NOTIFY displayChanged)
    Q_PROPERTY(QStringList displayedMedia READ displayedMedia
                   NOTIFY displayChanged)
    Q_PROPERTY(QStringList displayLeftMetrics READ displayLeftMetrics
                   NOTIFY displayChanged)
    Q_PROPERTY(QStringList displayRightMetrics READ displayRightMetrics
                   NOTIFY displayChanged)
    Q_PROPERTY(QStringList displayLeftBadges READ displayLeftBadges
                   NOTIFY displayChanged)
    Q_PROPERTY(QStringList displayRightBadges READ displayRightBadges
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayLeftPosition READ displayLeftPosition
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayLeftColor READ displayLeftColor
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayLeftAlignment READ displayLeftAlignment
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayRightPosition READ displayRightPosition
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayRightColor READ displayRightColor
                   NOTIFY displayChanged)
    Q_PROPERTY(QString displayRightAlignment READ displayRightAlignment
                   NOTIFY displayChanged)

public:
    explicit RuntimeClient(bool offline = false,
                           QObject *parent = nullptr);

    bool serviceAvailable() const;
    bool compatible() const;
    bool connected() const;
    bool ready() const;
    bool legacyConnected() const;
    bool printerClassDevicePresent() const;
    bool displaySessionActive() const;
    QString productId() const;
    QString deviceModel() const;
    QString firmwareVersion() const;
    QString deviceAppVersion() const;
    int mediaTargetWidth() const;
    int mediaTargetHeight() const;
    QString connectionStatus() const;
    QString diagnostic() const;
    quint32 apiVersion() const;
    quint32 expectedApiVersion() const;
    bool capabilitiesReady() const;
    QStringList runtimeCapabilities() const;
    bool supportSnapshotAvailable() const;
    bool supportSnapshotBusy() const;
    bool deviceCapabilitiesReady() const;
    QStringList deviceCapabilities() const;
    bool customBadgeTextSupported() const;
    QVariantMap displayBadgeChoices() const;
    Q_INVOKABLE QString badgeTextError(const QString &mode, const QString &text) const;
    bool deviceSpecificationsSupported() const;
    QString deviceSpecificationsStatus() const;
    bool deviceSpecificationsReady() const;
    QString deviceReportedProductName() const;
    int deviceVideoOutputWidth() const;
    int deviceVideoOutputHeight() const;
    QString deviceScreenType() const;
    bool deviceUsbAutoKeepalive() const;
    bool presentationPreferencesReady() const;
    bool presentationPreferencesBusy() const;
    QString temperatureUnit() const;
    QString timeFormat() const;
    SavedLayoutListModel *savedLayoutModel();
    bool savedLayoutsSupported() const;
    bool savedLayoutsReady() const;
    bool savedLayoutsBusy() const;
    QString savedLayoutsStatus() const;
    QString savedLayoutsDiagnostic() const;
    QString savedLayoutsDeviceIdentity() const;
    Q_INVOKABLE bool hasRuntimeCapability(
        const QString &capability) const;
    Q_INVOKABLE bool hasDeviceCapability(
        const QString &capability) const;
    Q_INVOKABLE bool requestSupportSnapshot();
    Q_INVOKABLE bool requestDeviceMediaMetadata(
        const TryxRuntimeDeviceMediaArtifact &artifact);
    Q_INVOKABLE QString formatTemperature(
        bool available, double celsius) const;
    Q_INVOKABLE void setPresentationPreferences(
        const QString &temperatureUnit,
        const QString &timeFormat);
    Q_INVOKABLE void refreshSavedLayouts();
    Q_INVOKABLE QVariantMap savedLayoutDraft(
        const QString &layoutId) const;
    Q_INVOKABLE QString savedLayoutIdForName(
        const QString &name) const;
    Q_INVOKABLE void putSavedLayout(
        const QString &name, const QString &overwriteLayoutId,
        const QVariantMap &fullDraft);
    Q_INVOKABLE void deleteSavedLayout(
        const QString &layoutId);
    Q_INVOKABLE QString submitSavedLayoutDraft(
        const QString &layoutId,
        const QString &revisionDecimal,
        const QVariantMap &fullDraft);
    MediaCatalogModel *mediaModel();
    OperationListModel *operationModel();

    bool operationBusy() const;
    QString activeOperationId() const;
    QString operationSummary() const;
    double operationProgress() const;

    QStringList availableMetrics() const;
    bool metricsCatalogReady() const;
    QStringList metricsCatalog() const;
    bool metricsEnabled() const;
    bool samplingActive() const;
    QStringList activeMetrics() const;
    QString metricsAlignment() const;
    QString metricsColor() const;

    bool displayStateValid() const;
    bool legacyDisplayLayoutConfirmed() const;
    bool legacyDisplayBrightnessConfirmed() const;
    quint64 displayRevision() const;
    QString displayDeviceIdentity() const;
    int brightness() const;
    bool backlightEnabled() const;
    bool mirrorMode() const;
    bool waterfallMode() const;
    QString currentScreenMode() const;
    QString currentPlayMode() const;
    QStringList displayedMedia() const;
    QStringList displayLeftMetrics() const;
    QStringList displayRightMetrics() const;
    QStringList displayLeftBadges() const;
    QStringList displayRightBadges() const;
    QString displayLeftPosition() const;
    QString displayLeftColor() const;
    QString displayLeftAlignment() const;
    QString displayRightPosition() const;
    QString displayRightColor() const;
    QString displayRightAlignment() const;

    QString queueUploadWithTransform(
        const QString &localPath,
        const TryxRuntimeMediaTransform &transform);
    QString queueUploadWithPreparationProfile(
        const QString &localPath,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString queueStageDeviceMedia(const QString &mediaId);
    void claimDeviceMediaArtifact(
        const QString &operationId, const QString &artifactId);
    void renewDeviceMediaArtifactLease(
        const QString &artifactId, const QString &leaseId);
    void releaseDeviceMediaArtifact(
        const QString &artifactId, const QString &leaseId);
    QString queueRecoveredMediaUploadWithTransform(
        const QString &artifactId, const QString &leaseId,
        const TryxRuntimeMediaTransform &transform);
    QString queueRecoveredMediaUploadWithPreparationProfile(
        const QString &artifactId, const QString &leaseId,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString queueReplaceDeviceMedia(
        const QString &artifactId, const QString &leaseId,
        const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform);
    QString queueReplaceDeviceMediaWithPreparationProfile(
        const QString &artifactId, const QString &leaseId,
        const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    TryxRuntimeApplyRequest currentDisplayApplyRequest() const;
    QString displayMediaReplacementBlockReason() const;
    QString queueCacheCleanup();
    bool cancelCacheCleanup(const QString &operationId);
    bool refreshCacheCleanup(const QString &operationId);
    void clearCacheCleanupTracking(const QString &operationId);

    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void refreshMedia();
    Q_INVOKABLE void connectDevice(const QString &port = QString());
    Q_INVOKABLE void disconnectDevice();
    Q_INVOKABLE void requestDeviceInfo();
    Q_INVOKABLE void setRotation(int degrees);
    Q_INVOKABLE void rebootDevice();
    Q_INVOKABLE void startKeepalive(int intervalSec);
    Q_INVOKABLE void stopKeepalive();
    Q_INVOKABLE void applyFullScreen(
        const QStringList &media, const QString &playMode,
        const QStringList &metrics, const QStringList &badges,
        const QString &position, const QString &color,
        const QString &alignment);
    Q_INVOKABLE void applySplitScreen(
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
        const QString &rightAlignment);
    Q_INVOKABLE QString submitFullDisplayDraft(
        const QStringList &media, const QString &playMode,
        const QStringList &metrics, const QStringList &badges,
        const QString &position, const QString &color,
        const QString &alignment, bool layoutPresent,
        bool brightnessPresent, int brightness,
        bool orientationPresent, bool mirror, bool waterfall,
        const QVariantMap &badgeChoices = QVariantMap());
    Q_INVOKABLE QString submitSplitDisplayDraft(
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
        bool mirror, bool waterfall, const QVariantMap &badgeChoices = QVariantMap());
    Q_INVOKABLE void abandonDisplaySubmission(
        const QString &submissionId);
    Q_INVOKABLE void deleteMedia(const QStringList &media);
    Q_INVOKABLE void cancelActiveOperation();
    Q_INVOKABLE void retryOperation(const QString &sourceOperationId);
    Q_INVOKABLE void configureMetrics(
        bool enabled, const QStringList &metrics,
        const QString &alignment, const QString &color);
    Q_INVOKABLE void setBrightness(int value);
    Q_INVOKABLE void setBacklight(bool enabled);
    Q_INVOKABLE void setOrientation(bool mirror, bool waterfall);
    void retranslate();

signals:
    void connectionChanged();
    void capabilitiesChanged();
    void deviceSpecificationsChanged();
    void supportSnapshotStateChanged();
    void supportSnapshotReady(const QString &json);
    void supportSnapshotFailed(const QString &message);
    void presentationPreferencesChanged();
    void savedLayoutsChanged();
    void savedLayoutPutFinished(
        const QString &requestedLayoutId,
        const QString &layoutId,
        const QString &revisionDecimal,
        bool success, const QString &message);
    void savedLayoutDeleteFinished(
        const QString &layoutId,
        bool success, const QString &message);
    void diagnosticChanged();
    void operationChanged();
    void metricsChanged();
    void metricsCatalogChanged();
    void displayChanged();
    void displayDeviceIdentityChanged();
    void userMessage(const QString &message, bool error);
    void operationRequestAccepted(const QString &operationId,
                                  const QString &kind);
    void operationRequestRejected(const QString &operationId,
                                  const QString &kind,
                                  const QString &message);
    void operationRequestFailed(const QString &operationId,
                                const QString &kind,
                                const QString &message,
                                bool outcomeUnknown);
    void operationUpdated(const TryxRuntimeOperationInfo &info);
    void cacheCleanupRefreshResolved(
        const TryxRuntimeOperationInfo &info);
    void cacheCleanupRefreshFailed(
        const QString &operationId, const QString &message);
    void displayApplyStarted(const QString &submissionId);
    void displayApplyFinished(const QString &submissionId,
                              const QString &outcome,
                              const QString &message);
    void artifactClaimed(
        const QString &operationId,
        const TryxRuntimeDeviceMediaArtifact &artifact);
    void artifactClaimFailed(const QString &operationId,
                             const QString &artifactId,
                             const QString &message);
    void deviceMediaMetadataReady(
        const TryxRuntimeDeviceMediaMetadataV1 &metadata);
    void deviceMediaMetadataFailed(
        const QString &artifactId, const QString &message);
    void artifactLeaseRenewed(const QString &artifactId,
                              const QString &leaseId);
    void artifactLeaseRenewFailed(const QString &artifactId,
                                  const QString &leaseId,
                                  const QString &message);
    void artifactReleased(const QString &artifactId,
                          const QString &leaseId);
    void artifactReleaseFailed(const QString &artifactId,
                               const QString &leaseId,
                               const QString &message);
    void runtimeInvalidated();

private slots:
    void onServiceRegistered(const QString &service);
    void onServiceUnregistered(const QString &service);
    void onOperationChanged(TryxRuntimeOperationInfo info,
                            quint64 revision);
    void onOperationRemoved(QString operationId, quint64 revision);
    void onMediaCatalogUpdated(
        TryxRuntimeMediaCatalogSnapshot snapshot);
    void onMetricsStateUpdated(TryxRuntimeMetricsState state);
    void onDisplayStateUpdated(TryxRuntimeDisplayState state);
    void onDisplaySnapshotChangedV1(quint64 revision);
    void onDeviceConnected(
        QString productId, QString serial, QString firmware,
        QString appVersion, bool printerClassConnected,
        bool printerClassDevicePresent, quint64 revision);
    void onDeviceDisconnected(quint64 revision);
    void onDeviceError(QString message, quint64 revision);
    void onLegacyBrightnessChanged(int value, quint64 revision);
    void onLegacyScreenConfigChanged(quint64 revision);
    void onLegacyMediaUploaded(QString filename, quint64 revision);
    void onLegacyMediaDeleted(quint64 revision);
    void onLegacyMediaListUpdated(QStringList files, quint64 revision);
    void onLegacyUploadStatus(QString status, quint64 revision);
    void onPrinterPresenceChanged(bool present,
                                  bool printerClassConnected,
                                  quint64 revision);
    void onPrinterOperationsCancelled(quint64 revision);
    void onDisplaySessionChanged(bool active, quint64 revision);
    void onPresentationPreferencesChangedV1(
        TryxRuntimePresentationPreferencesV1 preferences);
    void onLegacyUploadTimeout();
    void onDisplayApplyTimeout();

private:
    friend class QuickClientTests;
    friend class RuntimeClientHandshakeTests;

    enum class MetricArea {
        LiveConfiguration,
        Full,
        Left,
        Right,
    };

    enum class DeviceSpecificationsState {
        NotSupported,
        RuntimeUnavailable,
        Disconnected,
        Unsupported,
        Unavailable,
        Ready,
    };

    struct OfflineRequest {
        QString method;
        QVariantList arguments;
        QString operationId;
        QString kind;
    };

    struct LegacyUploadState {
        QString operationId;
        QString sourcePath;
        QString claimedPath;
        quint64 device = 0;
        quint64 inode = 0;
        quint64 epoch = 0;
        bool rejectionEmitted = false;

        bool active() const {
            return !operationId.isEmpty();
        }
    };

    struct DisplaySubmissionState {
        QString id;
        TryxRuntimeApplyRequest request;
        bool coherentSnapshot = false;
        TryxRuntimeOverlayBadgesV1 expectedBadges;
        quint64 physicalGeneration = 0;
        quint64 serviceEpoch = 0;
        quint64 handshakeAttempt = 0;
        QString owner;
        QString productId;
        QString deviceIdentity;
        quint64 startingDisplayRevision = 0;
        bool layoutPresent = false;
        bool brightnessPresent = false;
        bool orientationPresent = false;
        bool legacyScreenConfig = false;
        bool legacyBrightness = false;
        bool legacyRequestAcknowledged = false;
        bool legacyCandidateObserved = false;
        TryxRuntimeDisplayState legacyCandidateState;
        bool terminalSucceeded = false;
        bool matchingStateObserved = false;
        bool unresolved = false;
        bool resultEmitted = false;

        bool active() const {
            return !id.isEmpty();
        }
    };

    void subscribeSignals();
    void startHandshake();
    void startRuntimeCapabilitiesHandshake(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner);
    void requestMetricsCatalog(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner);
    void requestDeviceCapabilities(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner);
    bool reconcileConnectionRevision(quint64 observedRevision);
    void completeConnectionRevisionReconciliation();
    void requestDeviceSpecifications(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner);
    void requestPresentationPreferences(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner, bool reconciliation = false);
    void requestSavedLayouts(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner);
    bool subscribePresentationPreferencesSignal(
        const QString &owner);
    void disconnectPresentationPreferencesSignal();
    bool applyPresentationPreferencesSnapshot(
        const TryxRuntimePresentationPreferencesV1 &preferences);
    void clearPresentationPreferencesState();
    void clearSavedLayoutsState();
    void setSavedLayoutsUnavailable(const QString &diagnostic);
    QString savedLayoutConnectionIdentity() const;
    bool applySavedLayoutsSnapshot(
        const TryxRuntimeSavedLayoutsSnapshotV2 &snapshot,
        QString *errorMessage = nullptr);
    bool applySavedLayoutsSnapshot(const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot, QString *errorMessage = nullptr);
    bool savedLayoutsV2Supported() const;
    bool fullSavedLayoutDraftToRequest(
        const QVariantMap &fullDraft,
        TryxRuntimeApplyRequest *request,
        QString *errorMessage) const;
    bool savedLayoutFromDraft(
        const QString &name, const QString &overwriteLayoutId,
        const QVariantMap &fullDraft,
        TryxRuntimeSavedLayoutV2 *layout,
        QString *errorMessage) const;
    bool savedLayoutFromDraft(const QString &name, const QString &overwriteLayoutId, const QVariantMap &fullDraft,
                             TryxRuntimeSavedLayoutV1 *layout, QString *errorMessage) const;
    static bool parseSavedLayoutRevision(
        const QString &revisionDecimal,
        quint64 *revision);
    void invalidateSupportSnapshotRequest(bool notifyFailure);
    void invalidateDeviceMediaMetadataRequest(bool notifyFailure);
    bool handshakeContextIsCurrent(
        quint64 epoch, quint64 handshakeAttempt,
        const QString &owner) const;
    bool dbusSignalContextIsCurrent() const;
    QString currentRuntimeOwner() const;
    void clearCapabilityState();
    void clearMetricsCatalogState();
    void refreshLegacyMetricsCatalog();
    void clearDeviceCapabilityState();
    void clearDeviceSpecificationsState();
    void setDeviceSpecificationsState(
        DeviceSpecificationsState state,
        const TryxRuntimeDeviceSpecificationsV1 &specifications = {});
    void clearRuntimeState();
    void refreshConnection();
    void refreshOperations();
    void refreshMetrics();
    void refreshDisplay();
    void refreshDisplaySnapshotV1();
    bool usesDisplaySnapshotV1() const;
    bool displaySnapshotContextIsCurrent(const TryxRuntimeDisplaySnapshotV1 &snapshot) const;
    bool applyDisplaySnapshotV1(const TryxRuntimeDisplaySnapshotV1 &snapshot);
    bool displaySubmissionContextIsCurrent() const;
    bool normalizeBadgeDraft(const QVariantMap &draft, const TryxRuntimeApplyRequest &request,
                             TryxRuntimeOverlayBadgesV1 *badges) const;
    void setDiagnostic(const QString &message);
    bool mutationReady(const QString &action);
    bool manager1Ready(const QString &action,
                       bool requireConnected = true);
    QString legacyDeviceIdentity() const;
    QString legacyDisplayIdentity() const;
    QString nextOperationId() const;
    void sendOperation(const QString &method,
                       const QVariantList &arguments,
                       const QString &operationId,
                       const QString &kind);
    void reconcileOperationAcknowledgement(
        const QString &operationId, const QString &kind,
        const QString &initialError, quint64 epoch,
        quint64 handshakeAttempt, const QString &owner,
        bool unexpectedNonEmptyIdentity);
    void reportOperationRequestFailure(
        const QString &operationId, const QString &kind,
        const QString &message, bool outcomeUnknown);
    static bool operationAcknowledgementMatches(
        const QString &expectedOperationId,
        const QString &returnedOperationId,
        const TryxRuntimeOperationInfo &observedOperation);
    void sendVoidCall(const QString &interfaceName,
                      const QString &method,
                      const QVariantList &arguments = {});
    void sendLegacyScreenConfig(
        const TryxRuntimeApplyRequest &request);
    void sendLegacyDisplaySubmission(
        const QString &method, const QVariantList &arguments,
        const QString &submissionId);
    void acknowledgeLegacyDisplaySubmission(
        const QString &submissionId);
    void finishLegacyDisplayPreflightFailure(
        const QString &submissionId, const QString &message,
        bool runtimeInvalidated);
    static QVariantList legacyScreenConfigArguments(
        const TryxRuntimeApplyRequest &request);
    bool claimLegacyUploadSource(
        const QString &operationId, const QString &sourcePath,
        QString *errorMessage);
    void beginLegacyUpload(const QString &operationId);
    void finishLegacyUpload(const QString &filename);
    void rejectLegacyUpload(const QString &message,
                            bool restoreSource);
    void clearLegacyUpload(bool removeSource,
                           bool removeClaim);
    static bool fileIdentityMatches(
        const QString &path, quint64 device, quint64 inode);
    static void removeFileIfIdentityMatches(
        const QString &path, quint64 device, quint64 inode);
    TryxRuntimeApplyRequest baseApplyRequest() const;
    TryxRuntimeApplyRequest fullScreenApplyRequest(
        const QStringList &media, const QString &playMode,
        const QStringList &metrics,
        const QStringList &badges,
        const QString &position, const QString &color,
        const QString &alignment) const;
    TryxRuntimeApplyRequest splitScreenApplyRequest(
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
        const QString &rightAlignment) const;
    bool overlayStyleValid(
        const QString &position, const QString &color,
        const QString &alignment,
        QString *errorMessage) const;
    bool metricsSelectionValid(
        const QStringList &metrics, bool allowEmpty,
        MetricArea area,
        QString *errorMessage) const;
    QStringList confirmedMetricsForArea(MetricArea area) const;
    bool badgesSelectionValid(
        const QStringList &badges,
        QString *errorMessage) const;
    bool metricsConfigRequest(
        bool enabled, const QStringList &metrics,
        const QString &alignment, const QString &color,
        TryxRuntimeMetricsConfigRequest *request,
        QString *errorMessage) const;
    void applyDisplayMutation(
        const TryxRuntimeDisplayMutation &mutation);
    QString beginDisplaySubmission(
        const TryxRuntimeApplyRequest &request,
        bool layoutPresent, bool brightnessPresent,
        bool orientationPresent, bool legacyScreenConfig,
        bool legacyBrightness, const QString &method,
        const QString &savedLayoutId = QString(),
        quint64 savedLayoutRevision = 0, const QVariantMap &badgeChoices = QVariantMap());
    bool displaySubmissionMatches(
        const TryxRuntimeDisplayState &state) const;
    void observeDisplaySubmissionState(
        const TryxRuntimeDisplayState &state);
    void observeDisplaySubmissionOperation(
        const TryxRuntimeOperationInfo &info);
    void finishDisplaySubmission(
        const QString &outcome, const QString &message,
        bool unresolved = false);
    void applyOperationsSnapshot(
        const TryxRuntimeOperationsSnapshot &snapshot);
    void applyConnectionSnapshot(
        const TryxRuntimeSnapshot &snapshot);
    void applyMetricsState(const TryxRuntimeMetricsState &state);
    void applyDisplayState(const TryxRuntimeDisplayState &state);
    void updateActiveOperation(
        const TryxRuntimeOperationInfo &info);

    QDBusConnection bus_;
    QDBusServiceWatcher serviceWatcher_;
    bool signalsSubscribed_ = false;
    bool serviceAvailable_ = false;
    bool compatible_ = false;
    quint32 apiVersion_ = 0;
    QString runtimeOwner_;
    quint64 handshakeAttempt_ = 0;
    bool handshakePending_ = false;
    bool runtimeCapabilitiesPending_ = false;
    bool runtimeCapabilitiesFailed_ = false;
    bool capabilitiesReady_ = false;
    QStringList runtimeCapabilities_;
    bool metricsCatalogPending_ = false;
    bool metricsCatalogReady_ = false;
    bool metricsCatalogLegacyFallback_ = false;
    QStringList metricsCatalog_;
    bool supportSnapshotBusy_ = false;
    quint64 supportSnapshotAttempt_ = 0;
    bool deviceMediaMetadataPending_ = false;
    quint64 deviceMediaMetadataAttempt_ = 0;
    QString pendingDeviceMediaMetadataArtifactId_;
    quint64 deviceCapabilitiesAttempt_ = 0;
    int connectionRevisionRefreshes_ = 0;
    QString connectionRevisionDiagnostic_;
    bool deviceCapabilitiesReady_ = false;
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilitiesSnapshot_;
    QStringList deviceCapabilities_;
    quint64 deviceSpecificationsAttempt_ = 0;
    bool deviceSpecificationsPending_ = false;
    DeviceSpecificationsState deviceSpecificationsState_ =
        DeviceSpecificationsState::NotSupported;
    TryxRuntimeDeviceSpecificationsV1 deviceSpecificationsSnapshot_;
    TryxRuntimePresentationPreferencesV1 presentationPreferences_;
    bool presentationPreferencesReady_ = false;
    bool presentationPreferencesPending_ = false;
    bool presentationPreferencesBusy_ = false;
    quint64 presentationPreferencesReadAttempt_ = 0;
    quint64 presentationPreferencesMutationAttempt_ = 0;
    QString presentationPreferencesSignalOwner_;
    SavedLayoutListModel savedLayoutModel_;
    TryxRuntimeSavedLayoutsSnapshotV2 savedLayouts_;
    TryxRuntimeSavedLayoutsSnapshotV2 lastConfirmedSavedLayouts_;
    bool lastConfirmedSavedLayoutsReady_ = false;
    bool savedLayoutsBusy_ = false;
    quint64 savedLayoutsAttempt_ = 0;
    TryxRuntimeSnapshot connection_;
    TryxRuntimeMetricsState metrics_;
    TryxRuntimeDisplayState display_;
    TryxRuntimeDisplaySnapshotV1 displaySnapshot_;
    bool displaySnapshotRequired_ = false;
    quint64 displaySnapshotReadAttempt_ = 0;
    bool displaySnapshotReadPending_ = false;
    bool displaySnapshotReadAgain_ = false;
    bool displaySnapshotReadFailed_ = false;
    bool displayRevisionReceived_ = false;
    QString legacyDisplayStateIdentity_;
    bool legacyLayoutConfirmed_ = false;
    bool legacyBrightnessConfirmed_ = false;
    MediaCatalogModel mediaModel_;
    OperationListModel operationModel_;
    QString activeOperationId_;
    TryxRuntimeOperationInfo activeOperation_;
    LegacyUploadState legacyUpload_;
    QTimer legacyUploadDeadline_;
    DisplaySubmissionState displaySubmission_;
    QTimer displayApplyDeadline_;
    QString cacheCleanupOperationId_;
    QString cacheCleanupOwner_;
    TryxRuntimeApplyRequest pendingLegacyScreenConfig_;
    bool pendingLegacyScreenConfigValid_ = false;
    QString diagnostic_;
    quint64 serviceEpoch_ = 1;
    bool offline_ = false;
    QList<OfflineRequest> offlineRequests_;
};
