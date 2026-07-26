#pragma once

#include <QWidget>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QTimer>
#include <QColor>
#include <QSlider>
#include <QListWidget>
#include <QFrame>
#include <QProgressBar>
#include <QHash>
#include <QToolButton>
#include <QMenu>
#include <QWidgetAction>

#include <QRadioButton>
#include "systemmonitor.h"
#include "splitconfig.h"

class DeviceManager;
struct TryxRuntimeOperationInfo;
struct TryxRuntimeOperationsSnapshot;
struct TryxRuntimeMediaCatalogSnapshot;
struct TryxRuntimeMetricsState;
struct TryxRuntimeDisplayMutation;
struct TryxRuntimeApplyRequest;
struct TryxRuntimeDisplayState;

class PanoramaPage : public QWidget {
    Q_OBJECT
public:
    explicit PanoramaPage(DeviceManager *deviceMgr, QWidget *parent = nullptr);

    bool isMetricsRunning() const { return metricsRunning_; }

signals:
    void statusMessage(const QString &msg);
    void metricsRunningChanged(bool running);

public slots:
    void startMetrics();
    void stopMetrics();

private slots:
    void onChooseTextColor();
    void onUploadClicked();
    void onSetDisplayClicked();
    void onDeleteClicked();
    void onRefreshClicked();
    void onMediaListUpdated(const QStringList &files);
    void onMediaCatalogUpdated(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void onMediaUploaded(const QString &filename);
    void onMediaDeleted();
    void onUploadStatus(const QString &status);
    void onOperationChanged(const TryxRuntimeOperationInfo &info,
                            quint64 revision);
    void onMetricsStateUpdated(const TryxRuntimeMetricsState &state);
    void onDisplayStateUpdated(const TryxRuntimeDisplayState &state);
    void onRetryClicked();
    void onCancelClicked();
    void onScreenModeChanged();
    void onCustomSave();
    void onFileListContextMenu(const QPoint &pos);

    // Display settings
    void onBrightnessChanged(int value);

    // Metrics sending
    void onSendMetrics();

private:
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
    void setupUi();
    void setupCustomizationTab(QWidget *parent);
    void setupDisplaySettings();
    QString thumbnailCachePathForDeviceFile(const QString &fileName) const;
    QString localPreviewSourceForDeviceFile(const QString &fileName) const;
    void cacheThumbnailForDeviceFile(const QString &fileName, const QString &sourcePath);
    void applyCachedThumbnailToDeviceItem(const QString &fileName);
    void deleteDeviceItems(const QList<QListWidgetItem *> &items);
    QString startPrinterApply(const QStringList &media,
                              const QString &ratio,
                              const QString &playMode,
                              const QStringList &metrics = {},
                              bool updateMetrics = true);
    TryxRuntimeApplyRequest fullScreenApplyRequest(
        const QStringList &media, const QString &ratio,
        const QString &playMode, const QStringList &metrics,
        bool replaceOverlay) const;
    void submitDisplayMutation(
        const TryxRuntimeDisplayMutation &mutation);
    void submitPendingBrightness();
    void schedulePendingBrightness();
    void updateBrightnessPipelineFromState(
        const TryxRuntimeDisplayState &state);
    void finishBrightnessPipeline(bool keepPending);
    void selectDisplayMedia(const QStringList &media);
    void syncOperationPanel(const TryxRuntimeOperationsSnapshot &snapshot);
    QString operationStatusText(const TryxRuntimeOperationInfo &info) const;
    void setUploadBusy(bool busy);
    void updateActionAvailability();
    QStringList selectedDeviceMediaNames() const;
    void updateCustomMetricsButton();
    void savePageState();
    void restorePageState();

    DeviceManager *deviceMgr_;
    SystemMonitor *monitor_;
    QTimer *metricsTimer_;
    bool metricsRunning_ = false;
    bool uploadBusy_ = false;
    bool refreshPending_ = false;

    QWidget *operationPanel_;
    QLabel *operationStatusLabel_;

    // Metrics status
    QLabel *metricsStatusLabel_;

    // Full-screen overlay controls
    QComboBox *alignCombo_;
    QPushButton *textColorBtn_;
    QColor textColor_ = QColor("#DCDCDC");
    QCheckBox *cbCpuBadge_;
    QCheckBox *cbGpuBadge_;

    // Customization tab - file management
    QListWidget *fileList_;
    QComboBox *ratioCombo_;
    QComboBox *screenModeCombo_;
    QComboBox *playModeCombo_;
    QPushButton *uploadBtn_;
    QPushButton *setDisplayBtn_;
    QPushButton *deleteBtn_;
    QPushButton *refreshBtn_;
    QPushButton *retryBtn_;
    QPushButton *cancelBtn_;
    QLabel *dropZone_;
    QProgressBar *progressBar_;

    // Customization tab - Screen Splitting
    QRadioButton *fullScreenRadio_;
    QRadioButton *splitScreenRadio_;
    QWidget *fullScreenControls_;
    SplitConfigWidget *splitConfigWidget_;
    QPushButton *customSaveBtn_;
    QToolButton *customMetricsBtn_;
    QMenu *customMetricsMenu_;
    QList<QCheckBox *> customMetricCheckboxes_;

    QString pendingUploadSourcePath_;
    QString activeOperationId_;
    QString retryOperationId_;
    QString printerMetricsOperationId_;
    QStringList pendingPrinterMetrics_;
    QStringList activePrinterMetrics_;
    QStringList availablePrinterMetrics_;
    QStringList activeLegacyMetrics_;
    bool legacyMetricsStartPending_ = false;
    QHash<QString, QString> uploadSourcePaths_;

    // Display settings panel
    QSlider *brightnessSlider_;
    QLabel *brightnessLabel_;
    QCheckBox *cbDisplayOff_;
    QCheckBox *cbMirrorMode_;
    QCheckBox *cbWaterfallMode_;
    QString brightnessOperationId_;
    int brightnessOperationTarget_ = -1;
    int pendingBrightness_ = -1;
    quint64 brightnessBaseRevision_ = 0;
    bool brightnessOperationSucceeded_ = false;
    bool brightnessReadbackConfirmed_ = false;
    bool brightnessDispatchQueued_ = false;
    bool displayMutationReady_ = false;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
