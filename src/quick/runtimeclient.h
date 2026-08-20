#pragma once

#include "mediacatalogmodel.h"
#include "operationlistmodel.h"
#include "runtimecontract.h"

#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

class RuntimeClient final : public QObject {
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
    Q_PROPERTY(int mediaTargetWidth READ mediaTargetWidth
                   NOTIFY connectionChanged)
    Q_PROPERTY(int mediaTargetHeight READ mediaTargetHeight
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus
                   NOTIFY connectionChanged)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY diagnosticChanged)
    Q_PROPERTY(quint32 apiVersion READ apiVersion NOTIFY connectionChanged)
    Q_PROPERTY(quint32 expectedApiVersion READ expectedApiVersion CONSTANT)
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
    int mediaTargetWidth() const;
    int mediaTargetHeight() const;
    QString connectionStatus() const;
    QString diagnostic() const;
    quint32 apiVersion() const;
    quint32 expectedApiVersion() const;
    MediaCatalogModel *mediaModel();
    OperationListModel *operationModel();

    bool operationBusy() const;
    QString activeOperationId() const;
    QString operationSummary() const;
    double operationProgress() const;

    QStringList availableMetrics() const;
    bool metricsEnabled() const;
    bool samplingActive() const;
    QStringList activeMetrics() const;
    QString metricsAlignment() const;
    QString metricsColor() const;

    bool displayStateValid() const;
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

    QString queueUploadWithTransform(
        const QString &localPath,
        const TryxRuntimeMediaTransform &transform);
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
    QString queueReplaceDeviceMedia(
        const QString &artifactId, const QString &leaseId,
        const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform);
    TryxRuntimeApplyRequest currentDisplayApplyRequest() const;

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
        const QStringList &metrics, const QStringList &badges);
    Q_INVOKABLE void applySplitScreen(
        const QString &leftMedia, const QString &rightMedia,
        const QString &playMode, const QStringList &leftMetrics,
        const QStringList &rightMetrics,
        const QStringList &leftBadges,
        const QStringList &rightBadges);
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
    void diagnosticChanged();
    void operationChanged();
    void metricsChanged();
    void displayChanged();
    void userMessage(const QString &message, bool error);
    void operationRequestAccepted(const QString &operationId,
                                  const QString &kind);
    void operationRequestRejected(const QString &operationId,
                                  const QString &kind,
                                  const QString &message);
    void operationUpdated(const TryxRuntimeOperationInfo &info);
    void artifactClaimed(
        const QString &operationId,
        const TryxRuntimeDeviceMediaArtifact &artifact);
    void artifactClaimFailed(const QString &operationId,
                             const QString &artifactId,
                             const QString &message);
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
    void onDisplaySessionChanged(bool active, quint64 revision);
    void onLegacyUploadTimeout();

private:
    friend class QuickClientTests;

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

    void subscribeSignals();
    void startHandshake();
    void clearRuntimeState();
    void refreshConnection();
    void refreshOperations();
    void refreshMetrics();
    void refreshDisplay();
    void setDiagnostic(const QString &message);
    bool mutationReady(const QString &action);
    bool manager1Ready(const QString &action,
                       bool requireConnected = true);
    QString legacyDeviceIdentity() const;
    QString nextOperationId() const;
    void sendOperation(const QString &method,
                       const QVariantList &arguments,
                       const QString &operationId,
                       const QString &kind);
    void reconcileOperationAcknowledgement(
        const QString &operationId, const QString &kind,
        const QString &initialError, quint64 epoch);
    static bool operationAcknowledgementMatches(
        const QString &expectedOperationId,
        const QString &returnedOperationId,
        const TryxRuntimeOperationInfo &observedOperation);
    void sendVoidCall(const QString &interfaceName,
                      const QString &method,
                      const QVariantList &arguments = {});
    void sendLegacyScreenConfig(
        const TryxRuntimeApplyRequest &request);
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
        const QStringList &badges) const;
    TryxRuntimeApplyRequest splitScreenApplyRequest(
        const QString &leftMedia, const QString &rightMedia,
        const QStringList &leftMetrics,
        const QStringList &rightMetrics,
        const QStringList &leftBadges,
        const QStringList &rightBadges) const;
    bool metricsSelectionValid(
        const QStringList &metrics, bool allowEmpty,
        QString *errorMessage) const;
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
    TryxRuntimeSnapshot connection_;
    TryxRuntimeMetricsState metrics_;
    TryxRuntimeDisplayState display_;
    bool displayRevisionReceived_ = false;
    MediaCatalogModel mediaModel_;
    OperationListModel operationModel_;
    QString activeOperationId_;
    TryxRuntimeOperationInfo activeOperation_;
    LegacyUploadState legacyUpload_;
    QTimer legacyUploadDeadline_;
    TryxRuntimeApplyRequest pendingLegacyScreenConfig_;
    bool pendingLegacyScreenConfigValid_ = false;
    QString diagnostic_;
    quint64 serviceEpoch_ = 1;
    bool offline_ = false;
    QList<OfflineRequest> offlineRequests_;
};
