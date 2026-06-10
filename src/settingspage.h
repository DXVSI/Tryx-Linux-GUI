#pragma once

#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QToolButton>

class DeviceManager;

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
    void onResetSettings();
    void onSaveSettings();

private:
    void setupUi();
    void loadSettings();
    QString selectedLanguage() const;
    QString languageButtonText(const QString &language) const;
    QString nextLanguage(const QString &language) const;
    void setSelectedLanguage(const QString &language);

    DeviceManager *deviceMgr_;
    QComboBox *portCombo_;
    QToolButton *languageButton_;
    QString loadedLanguage_ = "system";
    QString selectedLanguage_ = "system";
    QSpinBox *keepaliveSpin_;
    QCheckBox *cbMinimizeToTray_;
    QCheckBox *cbStartMinimized_;
    QCheckBox *cbAutostart_;
    QPushButton *deviceInfoBtn_;
    QPushButton *resetBtn_;
    QPushButton *saveBtn_;
    QPushButton *refreshPortsBtn_;
};
