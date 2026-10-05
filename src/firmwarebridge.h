#pragma once

#include "firmwarerecoveryjournal.h"

#include <QDBusAbstractAdaptor>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <optional>

class DeviceManager;
class FirmwareUpdater;
class QTemporaryDir;
class PrinterProtocolTests;
class TryxRuntimeExportedObject;

class FirmwareBridge final : public QObject {
    Q_OBJECT

public:
    static constexpr quint32 InterfaceVersion = 2;

    explicit FirmwareBridge(DeviceManager *deviceManager,
                            QObject *parent = nullptr,
                            const QString &recoveryJournalPath = {});
    ~FirmwareBridge() override;

    QVariantMap stateForCaller(const QString &callerUniqueName) const;
    bool requestValidation(const QString &packagePath,
                           const QString &callerUniqueName);
    bool requestFlash(const QString &approvalToken,
                      const QString &callerUniqueName);
    void requestCancel(const QString &callerUniqueName);
    bool requestRecoveryAcknowledgement(
        const QString &callerUniqueName);
    bool shutdownInhibited() const { return shutdownInhibited_; }
    bool recoveryRequired() const {
        return recoveryRequired_;
    }
    void prepareForShutdown();

signals:
    void stateChanged(const QVariantMap &state);
    void progressChanged(int progress, const QString &message);
    void finished(bool success, const QString &message);
    void shutdownInhibitionChanged(bool inhibited);

private:
    struct Approval {
        QString token;
        QString ownerUniqueName;
        QString canonicalPath;
        QString sha256;
        QString kind;
        qint64 size = 0;
        qint64 mtimeUtcMs = 0;
        qint64 expiresUtcMs = 0;
    };

    void handleValidationResult(const QString &requestId,
                                const QString &callerUniqueName,
                                const QVariantMap &result);
    void handleFlashRevalidationResult(const QString &requestId,
                                       const QVariantMap &result,
                                       const QVariantMap &stagedResult,
                                       const std::shared_ptr<QTemporaryDir> &directory,
                                       const QString &stagedPath,
                                       const QString &stagingError);
    void handleFirmwareTransportQuiesced(
        const QString &leaseId, bool success,
        const QString &message);
    void handlePostQuiesceValidationResult(
        const QString &requestId,
        const QVariantMap &result);
    void startApprovedFlash(const Approval &approval);
    void handleUpdaterStatus(const QString &message);
    void handleUpdaterProgress(int progress);
    void handleUpdaterFinished(bool success, const QString &message);
    void failPendingFlash(const QString &message);
    void stopQuiesceDeadline();
    void releaseFirmwareGate(bool resumeTransport);
    void setShutdownInhibited(bool inhibited);
    static bool identityMatchesApproval(
        const Approval &approval,
        const QVariantMap &result);
    void setFailure(const QString &message);
    void publishState();
    QVariantMap publicState() const;
    bool hasActiveDeviceOperation(QString *operationId = nullptr) const;
    bool callerOwns(const QString &expectedOwner,
                    const QString &callerUniqueName) const;
    bool approvalExpired(const Approval &approval) const;
    static bool stageApprovedPackageCopy(
        const QString &sourcePath,
        qint64 expectedSize,
        const QString &expectedSha256,
        std::shared_ptr<QTemporaryDir> *directory,
        QString *stagedPath,
        QString *errorMessage);
    static bool stagedIdentityMatchesApproval(
        const Approval &approval,
        const QVariantMap &result);
    void clearStagedPackage();
    void loadRecoveryJournal();
    bool armRecoveryJournal(
        const Approval &approval,
        QString *errorMessage);
    bool updateRecoveryJournalPhase(
        const QString &phase,
        QString *errorMessage);
    bool clearCurrentAttemptRecoveryJournal(
        QString *errorMessage);
    QString recoveryStatusText() const;

    friend class PrinterProtocolTests;

    DeviceManager *deviceManager_ = nullptr;
    QThread firmwareThread_;
    QTimer quiesceDeadlineTimer_;
    QObject *firmwareThreadContext_ = nullptr;
    FirmwareUpdater *updater_ = nullptr;
    TryxFirmwareRecoveryJournal recoveryJournal_;
    bool workerReady_ = false;
    bool validationBusy_ = false;
    bool flashBusy_ = false;
    int progress_ = 0;
    QString phase_ = QStringLiteral("Initializing");
    QString status_;
    QString validationRequestId_;
    QString flashRevalidationRequestId_;
    QString postQuiesceValidationRequestId_;
    QString firmwareGateLeaseId_;
    QString flashOwnerUniqueName_;
    QString stagedPackagePath_;
    QVariantMap package_;
    std::optional<Approval> approval_;
    std::optional<Approval> pendingFlash_;
    std::shared_ptr<QTemporaryDir> stagedPackageDirectory_;
    bool updaterStarted_ = false;
    bool updaterIrreversibleStarted_ = false;
    bool recoveryRequired_ = false;
    bool recoveryJournalInvalid_ = false;
    bool attemptInheritedRecovery_ = false;
    bool currentAttemptJournalCreated_ = false;
    TryxFirmwareRecoveryRecord recoveryRecord_;
    std::optional<TryxFirmwareRecoveryRecord>
        pendingRecoveryRecord_;
    QString recoveryJournalError_;
    bool shutdownInhibited_ = false;
    bool shutdownRequested_ = false;
};

class FirmwareAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Firmware1")

public:
    FirmwareAdaptor(TryxRuntimeExportedObject *exportedObject,
                    FirmwareBridge *bridge);

public slots:
    quint32 GetFirmwareApiVersion() const;
    QVariantMap GetFirmwareState() const;
    bool ValidateFirmware(const QString &packagePath);
    bool StartFirmwareFlash(const QString &approvalToken);
    void CancelFirmware();
    bool AcknowledgeFirmwareRecovery();

signals:
    void StateChanged(const QVariantMap &state);
    void ProgressChanged(int progress, const QString &message);
    void Finished(bool success, const QString &message);

private:
    QString callerUniqueName() const;

    TryxRuntimeExportedObject *exportedObject_ = nullptr;
    FirmwareBridge *bridge_ = nullptr;
};

QString tryxFirmwareInterfaceName();
