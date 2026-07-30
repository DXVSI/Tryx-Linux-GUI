#pragma once

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

class FirmwareController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool serviceAvailable READ serviceAvailable
                   NOTIFY connectionChanged)
    Q_PROPERTY(bool compatible READ compatible NOTIFY connectionChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString packagePath READ packagePath WRITE setPackagePath
                   NOTIFY packagePathChanged)
    Q_PROPERTY(QUrl homeFolder READ homeFolder CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool validationBusy READ validationBusy NOTIFY stateChanged)
    Q_PROPERTY(bool flashBusy READ flashBusy NOTIFY stateChanged)
    Q_PROPERTY(bool approvalAvailable READ approvalAvailable
                   NOTIFY stateChanged)
    Q_PROPERTY(bool flashSupported READ flashSupported
                   NOTIFY stateChanged)
    Q_PROPERTY(bool canValidate READ canValidate
                   NOTIFY availabilityChanged)
    Q_PROPERTY(bool canFlash READ canFlash NOTIFY availabilityChanged)
    Q_PROPERTY(bool confirmationRequired READ confirmationRequired
                   NOTIFY confirmationRequiredChanged)
    Q_PROPERTY(int progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY stateChanged)
    Q_PROPERTY(QString canonicalPath READ canonicalPath NOTIFY stateChanged)
    Q_PROPERTY(QString sha256 READ sha256 NOTIFY stateChanged)
    Q_PROPERTY(qint64 sizeBytes READ sizeBytes NOTIFY stateChanged)
    Q_PROPERTY(QString productCode READ productCode NOTIFY stateChanged)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion
                   NOTIFY stateChanged)
    Q_PROPERTY(QString appVersion READ appVersion NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)

public:
    explicit FirmwareController(QObject *parent = nullptr);

    bool serviceAvailable() const;
    bool compatible() const;
    bool ready() const;
    QString packagePath() const;
    QUrl homeFolder() const;
    bool busy() const;
    bool validationBusy() const;
    bool flashBusy() const;
    bool approvalAvailable() const;
    bool flashSupported() const;
    bool canValidate() const;
    bool canFlash() const;
    bool confirmationRequired() const;
    int progress() const;
    QString phase() const;
    QString status() const;
    QString kind() const;
    QString canonicalPath() const;
    QString sha256() const;
    qint64 sizeBytes() const;
    QString productCode() const;
    QString firmwareVersion() const;
    QString appVersion() const;
    QString errorMessage() const;

    Q_INVOKABLE void setPackagePath(const QString &path);

    Q_INVOKABLE void validatePackage();
    Q_INVOKABLE void requestFlashConfirmation();
    Q_INVOKABLE void cancelFlashConfirmation();
    Q_INVOKABLE void confirmFlash();
    Q_INVOKABLE void requestCancel();
    Q_INVOKABLE void refresh();
    void retranslate();

signals:
    void connectionChanged();
    void packagePathChanged();
    void stateChanged();
    void availabilityChanged();
    void confirmationRequiredChanged();
    void finished(bool success, const QString &message);

private slots:
    void onServiceRegistered(const QString &service);
    void onServiceUnregistered(const QString &service);
    void onRemoteStateChanged(QVariantMap state);
    void onRemoteProgressChanged(int progress, QString message);
    void onRemoteFinished(bool success, QString message);

private:
    void subscribeSignals();
    void startHandshake();
    void requestState();
    void applyState(const QVariantMap &state, bool includeApproval);
    void clearRemoteState(const QString &status);
    void setRequestPending(bool pending);
    void setTransportError(const QString &message);
    void emitDerivedChanges();

    QDBusConnection bus_;
    QDBusServiceWatcher serviceWatcher_;
    bool signalsSubscribed_ = false;
    bool serviceAvailable_ = false;
    bool compatible_ = false;
    bool ready_ = false;
    bool remoteBusy_ = false;
    bool validationBusy_ = false;
    bool flashBusy_ = false;
    bool flashSupported_ = false;
    bool requestPending_ = false;
    bool stateRequestPending_ = false;
    bool stateRefreshAgain_ = false;
    bool confirmationRequired_ = false;
    int progress_ = 0;
    quint32 apiVersion_ = 0;
    quint64 serviceEpoch_ = 1;
    QString packagePath_;
    QString approvalSourcePath_;
    QString approvalToken_;
    QString phase_ = QStringLiteral("Unavailable");
    QString status_;
    QString kind_;
    QString canonicalPath_;
    QString sha256_;
    qint64 sizeBytes_ = 0;
    QString productCode_;
    QString firmwareVersion_;
    QString appVersion_;
    QString transportError_;
};
