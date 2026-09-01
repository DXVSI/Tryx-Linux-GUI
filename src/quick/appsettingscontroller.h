#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AppSettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(bool hideToTrayOnClose READ hideToTrayOnClose
                   NOTIFY closeBehaviorChanged)
    Q_PROPERTY(QString devicePort READ devicePort
                   NOTIFY deviceSettingsChanged)
    Q_PROPERTY(int keepaliveInterval READ keepaliveInterval
                   NOTIFY deviceSettingsChanged)
    Q_PROPERTY(QStringList serialPorts READ serialPorts
                   NOTIFY serialPortsChanged)
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled
                   NOTIFY autostartEnabledChanged)
    Q_PROPERTY(bool autostartAvailable READ autostartAvailable
                   NOTIFY autostartAvailableChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage
                   NOTIFY errorMessageChanged)

public:
    explicit AppSettingsController(bool offline = false,
                                   QObject *parent = nullptr);

    QString language() const;
    bool hideToTrayOnClose() const;
    QString devicePort() const;
    int keepaliveInterval() const;
    QStringList serialPorts() const;
    bool autostartEnabled() const;
    bool autostartAvailable() const;
    bool busy() const;
    QString errorMessage() const;

    Q_INVOKABLE void setLanguage(const QString &code);
    Q_INVOKABLE void setHideToTrayOnClose(bool enabled);
    Q_INVOKABLE void setDevicePort(const QString &port);
    Q_INVOKABLE void setKeepaliveInterval(int seconds);
    Q_INVOKABLE void refreshSerialPorts();
    Q_INVOKABLE void setAutostartEnabled(bool enabled);
    Q_INVOKABLE void refreshAutostart();

signals:
    void languageChanged();
    void closeBehaviorChanged();
    void deviceSettingsChanged();
    void serialPortsChanged();
    void autostartEnabledChanged();
    void autostartAvailableChanged();
    void busyChanged();
    void errorMessageChanged();

private:
    static bool isSupportedLanguage(const QString &code);
    bool saveDeviceSettings(const QString &port,
                            int keepaliveInterval);
    void setAutostartEnabledState(bool enabled);
    void setAutostartAvailableState(bool available);
    void setConfigError(const QString &message);
    void setAutostartError(const QString &message);
    void updateErrorMessage();

    bool offline_ = false;
    QString language_ = QStringLiteral("en");
    bool hideToTrayOnClose_ = true;
    QString devicePort_;
    int keepaliveInterval_ = 10;
    QStringList serialPorts_;
    bool autostartEnabled_ = false;
    bool autostartAvailable_ = false;
    bool busy_ = false;
    QString configError_;
    QString autostartError_;
    QString errorMessage_;
};
