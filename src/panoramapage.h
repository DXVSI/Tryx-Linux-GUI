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
#include <QScrollArea>
#include <QGridLayout>
#include <QFrame>
#include <QProgressBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QMap>
#include <QHash>
#include <QSettings>
#include <QToolButton>
#include <QMenu>
#include <QWidgetAction>

#include <QRadioButton>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>
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

struct MediaEntry;
class MediaTile;

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
    // Tab switching
    void onTabChanged(int index);

    // Pre-set tab
    void onMetricToggled();
    void onChooseTextColor();
    void onPresetSave();
    void onTileClicked(MediaTile *tile);

    // Customization tab
    void onUploadClicked();
    void onUploadBuiltinClicked();
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
    void setupPresetTab(QWidget *parent);
    void setupCustomizationTab(QWidget *parent);
    void setupDisplaySettings();
    void loadBuiltinMedia();
    QPixmap extractThumbnail(const QString &videoPath, const QString &cachePath);
    QString thumbnailCachePathForDeviceFile(const QString &fileName) const;
    QString builtinPreviewSourceForDeviceFile(const QString &fileName) const;
    QString localPreviewSourceForDeviceFile(const QString &fileName) const;
    void cacheThumbnailForDeviceFile(const QString &fileName, const QString &sourcePath);
    void applyCachedThumbnailToDeviceItem(const QString &fileName);
    void deleteDeviceItems(const QList<QListWidgetItem *> &items);
    void applyScreenConfig();
    QString startPrinterApply(const QStringList &media,
                              const QString &ratio,
                              const QString &playMode,
                              const QStringList &metrics = {},
                              const QString &presetId = QString(),
                              bool updateMetrics = true);
    TryxRuntimeApplyRequest fullScreenApplyRequest(
        const QStringList &media, const QString &ratio,
        const QString &playMode, const QStringList &metrics,
        const QString &presetId, bool replaceOverlay) const;
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
    void ensurePreviewPlayer();

    void rebuildPresetGrid();
    int calculateGridColumns() const;

    static QString builtinMediaDir();
    static QString presetIdForName(const QString &name);

    DeviceManager *deviceMgr_;
    SystemMonitor *monitor_;
    QTimer *metricsTimer_;
    bool metricsRunning_ = false;
    bool uploadBusy_ = false;
    bool refreshPending_ = false;

    // Tab bar
    QTabBar *tabBar_;
    QStackedWidget *tabStack_;
    QWidget *operationPanel_;
    QLabel *operationStatusLabel_;

    // Pre-set tab - built-in media carousel
    QScrollArea *presetScrollArea_;
    QWidget *presetGridWidget_;
    QGridLayout *presetGrid_;
    QList<MediaTile *> presetTiles_;
    MediaTile *selectedPresetTile_ = nullptr;
    QPushButton *presetSaveBtn_ = nullptr;

    // Preview
    QLabel *previewLabel_ = nullptr;
    QMediaPlayer *previewPlayer_ = nullptr;
    QVideoSink *previewSink_ = nullptr;

    // Pre-set tab - sysinfo display
    struct MetricOption {
        QCheckBox *checkbox;
        QString label;
        QString unit;
    };
    QList<MetricOption> metricOptions_;
    QLabel *selectionCountLabel_;

    // Display settings controls
    QComboBox *positionCombo_;
    QComboBox *alignCombo_;
    QPushButton *textColorBtn_;
    QColor textColor_ = QColor("#DCDCDC");
    QCheckBox *cbCpuBadge_;
    QCheckBox *cbGpuBadge_;

    // Metrics status
    QLabel *metricsStatusLabel_;

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

    // Customization tab - user media grid
    QScrollArea *customScrollArea_;
    QWidget *customGridWidget_;
    QGridLayout *customGrid_;
    QList<MediaTile *> customTiles_;
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
    void resizeEvent(QResizeEvent *event) override;
};
