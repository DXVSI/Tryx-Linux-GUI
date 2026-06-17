#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QString>
#include <functional>

class DeviceManager;
class Homepage;
class PanoramaPage;
class SettingsPage;
class TrayManager;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &currentLanguage,
                        std::function<void(const QString &)> languageHandler,
                        QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUi();
    void setupConnections();
    void setupPageConnections();
    void rebuildCentralUi();
    void onLanguageChanged(const QString &language);

    DeviceManager *deviceMgr_ = nullptr;
    Homepage *homepage_ = nullptr;
    PanoramaPage *panoramaPage_ = nullptr;
    SettingsPage *settingsPage_ = nullptr;
    TrayManager *trayMgr_ = nullptr;

    QStackedWidget *stack_ = nullptr;
    QListWidget *navList_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    std::function<void(const QString &)> languageHandler_;
    QString currentLanguage_;

    bool minimizeToTray_ = true;
};
