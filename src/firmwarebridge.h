#pragma once

#include <QDBusAbstractAdaptor>
#include <QObject>
#include <QThread>
#include <QVariantMap>

#include <optional>

class DeviceManager;
class FirmwareUpdater;
class TryxRuntimeExportedObject;

class FirmwareBridge final : public QObject {
    Q_OBJECT

public:
    static constexpr quint32 InterfaceVersion = 1;

    explicit FirmwareBridge(DeviceManager *deviceManager,
                            QObject *parent = nullptr);
    ~FirmwareBridge() override;

    QVariantMap stateForCaller(const QString &callerUniqueName) const;
    bool requestValidation(const QString &packagePath,
                           const QString &callerUniqueName);
    bool requestFlash(const QString &approvalToken,
                      const QString &callerUniqueName);
    void requestCancel(const QString &callerUniqueName);

signals:
    void stateChanged(const QVariantMap &state);
    void progressChanged(int progress, const QString &message);
    void finished(bool success, const QString &message);

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
                                       const QVariantMap &result);
    void startApprovedFlash(const Approval &approval);
    void handleUpdaterStatus(const QString &message);
    void handleUpdaterProgress(int progress);
    void handleUpdaterFinished(bool success, const QString &message);
    void setFailure(const QString &message);
    void publishState();
    QVariantMap publicState() const;
    bool hasActiveDeviceOperation(QString *operationId = nullptr) const;
    bool callerOwns(const QString &expectedOwner,
                    const QString &callerUniqueName) const;
    bool approvalExpired(const Approval &approval) const;

    DeviceManager *deviceManager_ = nullptr;
    QThread firmwareThread_;
    QObject *firmwareThreadContext_ = nullptr;
    FirmwareUpdater *updater_ = nullptr;
    bool workerReady_ = false;
    bool validationBusy_ = false;
    bool flashBusy_ = false;
    int progress_ = 0;
    QString phase_ = QStringLiteral("Initializing");
    QString status_;
    QString validationRequestId_;
    QString flashRevalidationRequestId_;
    QString flashOwnerUniqueName_;
    QVariantMap package_;
    std::optional<Approval> approval_;
    std::optional<Approval> pendingFlash_;
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
