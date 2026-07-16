#pragma once

#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QProcess>

class DeviceManager;
class FirmwareUpdater;
class QGroupBox;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(DeviceManager *deviceMgr, QWidget *parent = nullptr);

    QString selectedPort() const;
    int keepaliveInterval() const;
    bool minimizeToTray() const;
    bool startMinimized() const;

signals:
    void statusMessage(const QString &msg);
    void settingsChanged();

private slots:
    void onRefreshPorts();
    void onShowDeviceInfo();
    void onSelectFirmwarePackage();
    void onValidateFirmwarePackage();
    void onFlashFirmware();
    void onFirmwareStatusChanged(const QString &message);
    void onFirmwareProgressChanged(int value);
    void onFirmwareFinished(bool success, const QString &message);
    void onResetSettings();
    void onSaveSettings();
    void onAutostartCommandFinished(int exitCode,
                                    QProcess::ExitStatus exitStatus);

private:
    void setupUi();
    void loadSettings();
    void setFirmwareBusy(bool busy);
    void updateFirmwareControls();
    QString firmwareDependencyMessage(bool includeFlasher) const;
    void queryAutostartState();
    void startAutostartCommand(bool enable);

    enum class AutostartOperation {
        None,
        Query,
        Enable,
        Disable
    };

    DeviceManager *deviceMgr_;
    FirmwareUpdater *firmwareUpdater_;
    QProcess *autostartProcess_;
    QGroupBox *connectionGroup_;
    QComboBox *portCombo_;
    QSpinBox *keepaliveSpin_;
    QCheckBox *cbMinimizeToTray_;
    QCheckBox *cbStartMinimized_;
    QCheckBox *cbAutostart_;
    QPushButton *deviceInfoBtn_;
    QLabel *firmwarePackageLabel_;
    QLabel *firmwareStatusLabel_;
    QProgressBar *firmwareProgress_;
    QPushButton *selectFirmwareBtn_;
    QPushButton *validateFirmwareBtn_;
    QPushButton *flashFirmwareBtn_;
    QPushButton *resetBtn_;
    QPushButton *saveBtn_;
    QPushButton *refreshPortsBtn_;
    QString firmwarePackagePath_;
    bool firmwarePackageValidated_ = false;
    bool firmwarePackageFlashSupported_ = false;
    bool firmwarePackageNeedsRockchipFlasher_ = false;
    AutostartOperation autostartOperation_ = AutostartOperation::None;
    bool autostartEnabled_ = false;
};
