#include "panoramapage.h"
#include "displaypage.h"
#include "devicemanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QColorDialog>
#include <QDateTime>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QProcess>
#include <QPixmap>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFont>
#include <QSettings>
#include <QSignalBlocker>
#include <QUuid>
#include <memory>

static const int TILE_WIDTH = 250;
static const int TILE_IMG_HEIGHT = 140;
static const QString THUMB_CACHE_DIR = "/tmp/tryx-panorama/thumbnails";
static const int MEDIA_SIZE_ROLE = Qt::UserRole + 1;
static const int MEDIA_SOURCE_ROLE = Qt::UserRole + 2;
static const int MEDIA_READ_ONLY_ROLE = Qt::UserRole + 3;
static const int MEDIA_THUMBNAIL_KEY_ROLE = Qt::UserRole + 4;
static const int MEDIA_MANAGED_ORIGIN_ROLE = Qt::UserRole + 5;
static const int MEDIA_DELETE_ALLOWED_ROLE = Qt::UserRole + 6;
static const int MEDIA_DELETE_BLOCK_REASON_ROLE = Qt::UserRole + 7;

// Mapping from built-in video base names to device preset IDs
static const QMap<QString, QString> PRESET_MAP = {
    {"Cooling delivery",      "Pre-set 1: Cooling delivery"},
    {"Migration",             "Pre-set 2: Migration"},
    {"Quantum Time Capsule",  "Pre-set 3: Quantum time capsule"},
    {"Exo-Ecologies",         "Pre-set 4: Exo-Ecologies"},
    {"Racing",                "Pre-set 5: Racing"},
    {"Shuttle",               "Pre-set 6: Shuttle"},
    {"Gift of TRYX",          "Pre-set 7: Gift of TRYX"},
};

static const QMap<QString, QString> PRINTER_DEFAULT_PREVIEW_MAP = {
    {"default_01.mp4.h264_2240x1080", "Cooling delivery.webm"},
    {"default_02.mp4.h264_2240x1080", "Migration.webm"},
    {"default_03.mp4.h264_2240x1080", "Exo-Ecologies.webm"},
    {"default_04.mp4.h264_2240x1080", "Quantum Time Capsule.webm"},
    {"default_05.mp4.h264_2240x1080", "Racing.webm"},
    {"default_06.mp4.h264_2240x1080", "Shuttle.webm"},
};

PanoramaPage::PanoramaPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr) {

    monitor_ = new SystemMonitor(this);
    metricsTimer_ = new QTimer(this);

    setupUi();
    restorePageState();

    connect(metricsTimer_, &QTimer::timeout, this, &PanoramaPage::onSendMetrics);

    // Device signals
    connect(deviceMgr_, &DeviceManager::mediaListUpdated, this, &PanoramaPage::onMediaListUpdated);
    connect(deviceMgr_, &DeviceManager::mediaCatalogUpdated, this,
            &PanoramaPage::onMediaCatalogUpdated);
    connect(deviceMgr_, &DeviceManager::mediaUploaded, this, &PanoramaPage::onMediaUploaded);
    connect(deviceMgr_, &DeviceManager::mediaDeleted, this, &PanoramaPage::onMediaDeleted);
    connect(deviceMgr_, &DeviceManager::uploadStatus, this, &PanoramaPage::onUploadStatus);
    connect(deviceMgr_, &DeviceManager::operationChanged, this,
            &PanoramaPage::onOperationChanged);
    connect(deviceMgr_, &DeviceManager::operationSnapshotUpdated, this,
            [this](const TryxRuntimeOperationsSnapshot &snapshot) {
                syncOperationPanel(snapshot);
            });
    connect(deviceMgr_, &DeviceManager::operationRemoved, this,
            [this](const QString &, quint64) {
                syncOperationPanel(deviceMgr_->operationSnapshot());
            });
    connect(deviceMgr_, &DeviceManager::metricsStateUpdated, this,
            &PanoramaPage::onMetricsStateUpdated);
    connect(deviceMgr_, &DeviceManager::displayStateUpdated, this,
            &PanoramaPage::onDisplayStateUpdated);
    connect(deviceMgr_, &DeviceManager::printerDisplaySessionChanged, this,
            [this](bool active) {
                if (!active) {
                    refreshPending_ = false;
                    displayMutationReady_ = false;
                    finishBrightnessPipeline(false);
                }
                updateActionAvailability();
            });
    connect(deviceMgr_, &DeviceManager::printerTransportReady, this,
            [this]() {
                if (!deviceMgr_->isPrinterClassDevicePresent() ||
                    !deviceMgr_->isPrinterDisplaySessionActive() ||
                    !deviceMgr_->displayState().valid ||
                    !deviceMgr_->operationSnapshot()
                         .activeOperationId.isEmpty()) {
                    return;
                }
                displayMutationReady_ = true;
                updateActionAvailability();
                schedulePendingBrightness();
            });
    connect(deviceMgr_, &DeviceManager::printerPresenceChanged, this,
            [this](bool present) {
                if (!present) {
                    refreshPending_ = false;
                    displayMutationReady_ = false;
                    finishBrightnessPipeline(false);
                }
                updateActionAvailability();
            });
    connect(deviceMgr_, &DeviceManager::deviceError, this,
            [this](const QString &message) {
                refreshPending_ = false;
                emit statusMessage(message);
                updateActionAvailability();
            });
    connect(deviceMgr_, &DeviceManager::brightnessChanged, this,
            [this](int val) {
                if (deviceMgr_->isPrinterClassDevicePresent()) {
                    return;
                }
                const QSignalBlocker blocker(brightnessSlider_);
                brightnessSlider_->setValue(val);
                brightnessLabel_->setText(QString::number(val));
            });
    connect(deviceMgr_, &DeviceManager::screenConfigChanged, this, [this]() {
        if (legacyMetricsStartPending_ &&
            !deviceMgr_->isPrinterClassDevicePresent()) {
            legacyMetricsStartPending_ = false;
            startMetrics();
        }
    });

    syncOperationPanel(deviceMgr_->operationSnapshot());
    if (deviceMgr_->hasTypedMediaCatalog()) {
        onMediaCatalogUpdated(deviceMgr_->mediaCatalogSnapshot());
    }
    onMetricsStateUpdated(deviceMgr_->metricsState());
    onDisplayStateUpdated(deviceMgr_->displayState());
    updateActionAvailability();
}

QString PanoramaPage::builtinMediaDir() {
    return DisplayPage::builtinMediaDir();
}

QString PanoramaPage::presetIdForName(const QString &name) {
    return PRESET_MAP.value(name);
}

QPixmap PanoramaPage::extractThumbnail(const QString &videoPath, const QString &cachePath) {
    if (QFileInfo::exists(cachePath)) {
        return QPixmap(cachePath);
    }
    QDir().mkpath(QFileInfo(cachePath).absolutePath());

    // Launch ffmpeg asynchronously to avoid blocking the GUI thread.
    // The process is parented to this widget so it gets cleaned up automatically.
    auto *proc = new QProcess(this);
    proc->start("ffmpeg", {"-y", "-i", videoPath,
                            "-vf", "select=eq(n\\,0),scale=384:-1",
                            "-frames:v", "1", "-q:v", "5", cachePath});

    // When the process finishes, find the matching tile and update its thumbnail
    QString cachedPath = cachePath;
    QString videoFilePath = videoPath;
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, cachedPath, videoFilePath, proc](int exitCode, QProcess::ExitStatus) {
                proc->deleteLater();
                if (exitCode == 0 && QFileInfo::exists(cachedPath)) {
                    QPixmap thumb(cachedPath);
                    // Find the tile matching this video and update its thumbnail
                    for (auto *tile : presetTiles_) {
                        if (tile->filePath() == videoFilePath) {
                            tile->setThumbnail(thumb);
                            break;
                        }
                    }
                }
            });

    // Return empty pixmap immediately; tile will be updated when ffmpeg finishes
    return {};
}

QString PanoramaPage::thumbnailCachePathForDeviceFile(const QString &fileName) const {
    QString key = QFileInfo(fileName).fileName();
    if (key.isEmpty()) {
        key = fileName;
    }
    const QString defaultPreviewSource = PRINTER_DEFAULT_PREVIEW_MAP.value(key);
    if (!defaultPreviewSource.isEmpty()) {
        key += QStringLiteral("__") + defaultPreviewSource;
    }
    key.replace(QLatin1Char(' '), QLatin1Char('_'));
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return THUMB_CACHE_DIR + QLatin1Char('/') + key + QStringLiteral(".jpg");
}

QString PanoramaPage::builtinPreviewSourceForDeviceFile(const QString &fileName) const {
    const QString mediaName =
        PRINTER_DEFAULT_PREVIEW_MAP.value(QFileInfo(fileName).fileName());
    if (mediaName.isEmpty()) {
        return {};
    }

    const QString mediaDir = builtinMediaDir();
    if (mediaDir.isEmpty()) {
        return {};
    }

    const QString sourcePath = QDir(mediaDir).absoluteFilePath(mediaName);
    return QFileInfo::exists(sourcePath) ? sourcePath : QString();
}

QString PanoramaPage::localPreviewSourceForDeviceFile(const QString &fileName) const {
    const QString localPath = QDir(QFileInfo(THUMB_CACHE_DIR).absolutePath())
                                  .absoluteFilePath(QFileInfo(fileName).fileName());
    return QFileInfo::exists(localPath) ? localPath : QString();
}

void PanoramaPage::applyCachedThumbnailToDeviceItem(const QString &fileName) {
    const QString cachePath = thumbnailCachePathForDeviceFile(fileName);
    if (!QFileInfo::exists(cachePath)) {
        return;
    }

    QPixmap pix(cachePath);
    if (pix.isNull()) {
        return;
    }

    const QString normalizedName = QFileInfo(fileName).fileName();
    for (int i = 0; i < fileList_->count(); ++i) {
        auto *item = fileList_->item(i);
        const QString itemName = QFileInfo(item->data(Qt::UserRole).toString()).fileName();
        if (itemName == normalizedName) {
            item->setIcon(QIcon(pix.scaled(120, 80,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation)));
            return;
        }
    }
}

void PanoramaPage::cacheThumbnailForDeviceFile(const QString &fileName,
                                               const QString &sourcePath) {
    if (fileName.isEmpty() || sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
        return;
    }

    const QString cachePath = thumbnailCachePathForDeviceFile(fileName);
    if (QFileInfo::exists(cachePath)) {
        applyCachedThumbnailToDeviceItem(fileName);
        return;
    }

    QDir().mkpath(QFileInfo(cachePath).absolutePath());

    QPixmap image(sourcePath);
    if (!image.isNull()) {
        image.scaled(384, 216, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .save(cachePath, "JPG", 85);
        applyCachedThumbnailToDeviceItem(fileName);
        return;
    }

    QStringList args = {"-y"};
    if (QFileInfo(sourcePath).fileName().contains(QStringLiteral(".h264_"))) {
        args << "-f" << "h264" << "-framerate" << "30";
    }
    args << "-i" << sourcePath
         << "-vf" << "select=eq(n\\,0),scale=384:-1"
         << "-frames:v" << "1"
         << "-q:v" << "5"
         << cachePath;

    auto *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, fileName, cachePath, proc](int exitCode, QProcess::ExitStatus) {
                proc->deleteLater();
                if (exitCode == 0 && QFileInfo::exists(cachePath)) {
                    applyCachedThumbnailToDeviceItem(fileName);
                }
            });
    proc->start("ffmpeg", args);
}

void PanoramaPage::setupUi() {
    setAcceptDrops(true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Header
    auto *headerWidget = new QWidget;
    headerWidget->setStyleSheet("background: #1e1e2e;");
    auto *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(20, 12, 20, 12);

    auto *titleLabel = new QLabel(tr("PANORAMA"));
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setStyleSheet("color: #fff;");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    // Tab bar in header
    tabBar_ = new QTabBar;
    tabBar_->addTab(tr("Pre-set"));
    tabBar_->addTab(tr("Customization"));
    tabBar_->setStyleSheet(
        "QTabBar::tab {"
        "  background: transparent;"
        "  color: #888;"
        "  padding: 8px 20px;"
        "  border: none;"
        "  font-size: 13px;"
        "}"
        "QTabBar::tab:selected {"
        "  color: #fff;"
        "  border-bottom: 2px solid #6c5ce7;"
        "}"
        "QTabBar::tab:hover {"
        "  color: #ccc;"
        "}");
    headerLayout->addWidget(tabBar_);
    headerLayout->addStretch();

    mainLayout->addWidget(headerWidget);

    // Tab stack
    tabStack_ = new QStackedWidget;

    auto *presetWidget = new QWidget;
    setupPresetTab(presetWidget);
    tabStack_->addWidget(presetWidget);

    auto *customWidget = new QWidget;
    setupCustomizationTab(customWidget);
    tabStack_->addWidget(customWidget);

    mainLayout->addWidget(tabStack_, 1);

    operationPanel_ = new QFrame;
    operationPanel_->setStyleSheet(
        "QFrame { background: #252532; border-top: 1px solid #444; }"
        "QLabel { color: #ddd; }");
    auto *operationLayout = new QHBoxLayout(operationPanel_);
    operationLayout->setContentsMargins(20, 8, 20, 8);
    operationLayout->setSpacing(10);

    operationStatusLabel_ = new QLabel;
    operationStatusLabel_->setWordWrap(true);
    operationStatusLabel_->setMinimumWidth(260);
    operationLayout->addWidget(operationStatusLabel_, 1);

    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 0);
    progressBar_->setMaximumHeight(20);
    progressBar_->setMinimumWidth(220);
    progressBar_->hide();
    operationLayout->addWidget(progressBar_);

    retryBtn_ = new QPushButton(tr("Retry transfer"));
    retryBtn_->hide();
    operationLayout->addWidget(retryBtn_);

    cancelBtn_ = new QPushButton(tr("Cancel"));
    cancelBtn_->hide();
    operationLayout->addWidget(cancelBtn_);

    operationPanel_->hide();
    mainLayout->addWidget(operationPanel_);

    connect(retryBtn_, &QPushButton::clicked, this,
            &PanoramaPage::onRetryClicked);
    connect(cancelBtn_, &QPushButton::clicked, this,
            &PanoramaPage::onCancelClicked);

    // Display settings panel at bottom
    setupDisplaySettings();

    connect(tabBar_, &QTabBar::currentChanged, this, &PanoramaPage::onTabChanged);
}

void PanoramaPage::onTabChanged(int index) {
    tabStack_->setCurrentIndex(index);
}

void PanoramaPage::setupPresetTab(QWidget *parent) {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto *scrollWidget = new QWidget;
    auto *layout = new QVBoxLayout(scrollWidget);
    layout->setSpacing(16);
    layout->setContentsMargins(20, 16, 20, 16);

    // Built-in media carousel
    auto *mediaLabel = new QLabel(tr("Built-in Media Library"));
    QFont mlFont = mediaLabel->font();
    mlFont.setPointSize(12);
    mlFont.setBold(true);
    mediaLabel->setFont(mlFont);
    mediaLabel->setStyleSheet("color: #fff;");
    layout->addWidget(mediaLabel);

    // Video preview via QVideoSink -> QLabel (no native window, works on Wayland)
    previewLabel_ = new QLabel;
    previewLabel_->setFixedSize(420, 200);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setStyleSheet("background: #000; border-radius: 8px; border: none;");
    previewLabel_->hide();
    layout->addWidget(previewLabel_, 0, Qt::AlignCenter);

    presetGridWidget_ = new QWidget;
    presetGrid_ = new QGridLayout(presetGridWidget_);
    presetGrid_->setSpacing(8);
    presetGrid_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(presetGridWidget_);

    loadBuiltinMedia();

    // System Information Display
    auto *siLabel = new QLabel(tr("System Information Display | Select up to 3 items"));
    QFont siFont = siLabel->font();
    siFont.setPointSize(11);
    siFont.setBold(true);
    siLabel->setFont(siFont);
    siLabel->setStyleSheet("color: #fff; margin-top: 8px;");
    layout->addWidget(siLabel);

    struct MetricDef {
        const char *displayName;
        QString protocolLabel;
        QString unit;
    };

    QList<MetricDef> defs = {
        {QT_TR_NOOP("CPU Temperature"),        "CPU Temperature",        "°C"},
        {QT_TR_NOOP("CPU Frequency"),          "CPU Frequency",          "MHZ"},
        {QT_TR_NOOP("CPU Usage"),              "CPU Usage",              "%"},
        {QT_TR_NOOP("CPU Power"),              "CPU Power",              "W"},
        {QT_TR_NOOP("GPU Temperature"),        "GPU Temperature",        "°C"},
        {QT_TR_NOOP("GPU Frequency"),          "GPU Frequency",          "MHZ"},
        {QT_TR_NOOP("GPU Usage"),              "GPU Usage",              "%"},
        {QT_TR_NOOP("GPU Power"),              "GPU Power",              "W"},
        {QT_TR_NOOP("Memory Frequency"),       "Memory Frequency",       "MHZ"},
        {QT_TR_NOOP("Memory Usage"),           "Memory Usage",           "%"},
        {QT_TR_NOOP("Date&Time"),              "Date&Time",              ""},
    };

    auto *metricsGrid = new QGridLayout;
    metricsGrid->setSpacing(4);
    int row = 0, col = 0;
    for (const auto &def : defs) {
        auto *cb = new QCheckBox(tr(def.displayName));
        cb->setStyleSheet("color: #ccc;");
        metricsGrid->addWidget(cb, row, col);

        MetricOption opt;
        opt.checkbox = cb;
        opt.label = def.protocolLabel;
        opt.unit = def.unit;
        metricOptions_.append(opt);

        connect(cb, &QCheckBox::toggled, this, &PanoramaPage::onMetricToggled);

        col++;
        if (col >= 4) { col = 0; row++; }
    }
    layout->addLayout(metricsGrid);

    selectionCountLabel_ = new QLabel(tr("Selected: 0 / 3"));
    selectionCountLabel_->setStyleSheet("color: #888;");
    layout->addWidget(selectionCountLabel_);

    // Display settings controls (position, color, align, badges)
    auto *controlsLayout = new QHBoxLayout;
    controlsLayout->setSpacing(12);

    auto *positionLabel = new QLabel(tr("Position:"));
    controlsLayout->addWidget(positionLabel);
    positionCombo_ = new QComboBox;
    positionCombo_->addItem(tr("Top"), "Top");
    positionCombo_->addItem(tr("Center"), "Center");
    positionCombo_->addItem(tr("Bottom"), "Bottom");
    controlsLayout->addWidget(positionCombo_);
    positionLabel->hide();
    positionCombo_->hide();

    controlsLayout->addWidget(new QLabel(tr("Align:")));
    alignCombo_ = new QComboBox;
    alignCombo_->addItem(tr("Left"), "Left");
    alignCombo_->addItem(tr("Center"), "Center");
    alignCombo_->addItem(tr("Right"), "Right");
    controlsLayout->addWidget(alignCombo_);

    textColorBtn_ = new QPushButton(tr("Color"));
    textColorBtn_->setObjectName(
        QStringLiteral("presetTextColorButton"));
    textColorBtn_->setStyleSheet("background-color: #DCDCDC; color: #000; padding: 4px 12px;");
    textColorBtn_->setMaximumWidth(80);
    connect(textColorBtn_, &QPushButton::clicked, this, &PanoramaPage::onChooseTextColor);
    controlsLayout->addWidget(textColorBtn_);

    cbCpuBadge_ = new QCheckBox(tr("CPU Badge"));
    cbCpuBadge_->setObjectName(
        QStringLiteral("presetCpuBadgeCheckBox"));
    cbCpuBadge_->setStyleSheet("color: #ccc;");
    cbGpuBadge_ = new QCheckBox(tr("GPU Badge"));
    cbGpuBadge_->setObjectName(
        QStringLiteral("presetGpuBadgeCheckBox"));
    cbGpuBadge_->setStyleSheet("color: #ccc;");
    controlsLayout->addWidget(cbCpuBadge_);
    controlsLayout->addWidget(cbGpuBadge_);

    controlsLayout->addStretch();
    layout->addLayout(controlsLayout);

    // Save button
    auto *sendLayout = new QHBoxLayout;
    sendLayout->addStretch();

    presetSaveBtn_ = new QPushButton(tr("Save"));
    presetSaveBtn_->setMinimumHeight(36);
    presetSaveBtn_->setMinimumWidth(120);
    presetSaveBtn_->setStyleSheet(
        "QPushButton { background: #00b894; color: white; border: none; border-radius: 4px; padding: 8px 24px; font-weight: bold; font-size: 13px; }"
        "QPushButton:hover { background: #00a381; }");
    connect(presetSaveBtn_, &QPushButton::clicked, this,
            &PanoramaPage::onPresetSave);
    sendLayout->addWidget(presetSaveBtn_);

    layout->addLayout(sendLayout);

    metricsStatusLabel_ = new QLabel("");
    metricsStatusLabel_->setStyleSheet("color: #888;");
    layout->addWidget(metricsStatusLabel_);

    layout->addStretch();

    scroll->setWidget(scrollWidget);

    auto *parentLayout = new QVBoxLayout(parent);
    parentLayout->setContentsMargins(0, 0, 0, 0);
    parentLayout->addWidget(scroll);
}

void PanoramaPage::setupCustomizationTab(QWidget *parent) {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border: none; }");

    auto *scrollWidget = new QWidget;
    auto *layout = new QVBoxLayout(scrollWidget);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    // Mode radio buttons
    auto *radioLayout = new QHBoxLayout;
    radioLayout->setSpacing(16);
    fullScreenRadio_ = new QRadioButton(tr("Full Screen"));
    fullScreenRadio_->setObjectName(
        QStringLiteral("fullScreenRadioButton"));
    splitScreenRadio_ = new QRadioButton(tr("Screen Splitting"));
    splitScreenRadio_->setObjectName(
        QStringLiteral("splitScreenRadioButton"));
    fullScreenRadio_->setChecked(true);
    fullScreenRadio_->setStyleSheet("color: #ccc;");
    splitScreenRadio_->setStyleSheet("color: #ccc;");
    radioLayout->addWidget(fullScreenRadio_);
    radioLayout->addWidget(splitScreenRadio_);
    radioLayout->addStretch();
    layout->addLayout(radioLayout);

    connect(fullScreenRadio_, &QRadioButton::toggled, this, &PanoramaPage::onScreenModeChanged);

    // --- Full Screen controls (existing) ---
    fullScreenControls_ = new QWidget;
    auto *fsLayout = new QVBoxLayout(fullScreenControls_);
    fsLayout->setContentsMargins(0, 0, 0, 0);
    fsLayout->setSpacing(8);

    auto *modeLayout = new QHBoxLayout;
    modeLayout->setSpacing(12);

    screenModeCombo_ = new QComboBox(fullScreenControls_);
    screenModeCombo_->addItem(tr("Full Screen"), "Full Screen");
    screenModeCombo_->addItem(tr("Screen Splitting"), "Screen Splitting");
    screenModeCombo_->hide(); // hidden, mode is now via radio buttons

    modeLayout->addWidget(new QLabel(tr("Play Mode:")));
    playModeCombo_ = new QComboBox;
    playModeCombo_->addItem(tr("Single"), "Single");
    playModeCombo_->addItem(tr("Shuffle"), "Shuffle");
    playModeCombo_->addItem(tr("Loop"), "Loop");
    modeLayout->addWidget(playModeCombo_);

    modeLayout->addWidget(new QLabel(tr("Ratio:")));
    ratioCombo_ = new QComboBox;
    ratioCombo_->addItems({"2:1", "1:1"});
    modeLayout->addWidget(ratioCombo_);

    modeLayout->addStretch();
    fsLayout->addLayout(modeLayout);

    // System info metrics for Full Screen
    auto *fsMetricsLabel = new QLabel(tr("System info:"));
    fsMetricsLabel->setStyleSheet("color: #aaa; font-size: 11px;");

    customMetricsBtn_ = new QToolButton;
    customMetricsBtn_->setText(QString::fromUtf8("0 / 3 \u25BC"));
    customMetricsBtn_->setPopupMode(QToolButton::InstantPopup);
    customMetricsBtn_->setStyleSheet(
        "QToolButton { background: #2a2a3e; color: #fff; border: 1px solid #4a4a5e; "
        "border-radius: 4px; padding: 6px 12px; min-width: 80px; font-size: 12px; } "
        "QToolButton::menu-indicator { image: none; } "
        "QToolButton:hover { background: #3a3a4e; }");

    customMetricsMenu_ = new QMenu(this);
    const char *metricLabels[] = {
        QT_TR_NOOP("CPU Temperature"), QT_TR_NOOP("CPU Frequency"),
        QT_TR_NOOP("CPU Usage"), QT_TR_NOOP("CPU Power"),
        QT_TR_NOOP("GPU Temperature"), QT_TR_NOOP("GPU Frequency"),
        QT_TR_NOOP("GPU Usage"), QT_TR_NOOP("GPU Power"),
        QT_TR_NOOP("Memory Frequency"), QT_TR_NOOP("Memory Usage"),
        QT_TR_NOOP("Date&Time")
    };
    for (const auto *label : metricLabels) {
        auto *wa = new QWidgetAction(customMetricsMenu_);
        auto *cb = new QCheckBox(tr(label));
        cb->setProperty("protocolLabel", label);
        cb->setStyleSheet("QCheckBox { color: #fff; padding: 4px 8px; } QCheckBox:hover { background: #3a3a4e; }");
        wa->setDefaultWidget(cb);
        customMetricsMenu_->addAction(wa);
        customMetricCheckboxes_.append(cb);
        connect(cb, &QCheckBox::toggled, this, [this](bool) {
            int count = 0;
            for (auto *c : customMetricCheckboxes_)
                if (c->isChecked()) count++;
            if (count > 3) {
                auto *s = qobject_cast<QCheckBox *>(QObject::sender());
                if (s) s->setChecked(false);
                return;
            }
            updateCustomMetricsButton();
        });
    }
    customMetricsBtn_->setMenu(customMetricsMenu_);

    auto *metricsRow = new QHBoxLayout;
    metricsRow->addWidget(fsMetricsLabel);
    metricsRow->addWidget(customMetricsBtn_);
    metricsRow->addStretch();
    fsLayout->addLayout(metricsRow);

    layout->addWidget(fullScreenControls_);

    // --- Screen Splitting controls ---
    splitConfigWidget_ = new SplitConfigWidget;
    splitConfigWidget_->hide();
    layout->addWidget(splitConfigWidget_);

    // Drop zone
    dropZone_ = new QLabel(tr("Upload a file\n(MP4, WEBM, GIF, JPG, PNG)"));
    dropZone_->setAlignment(Qt::AlignCenter);
    dropZone_->setMinimumHeight(80);
    dropZone_->setStyleSheet(
        "QLabel {"
        "  border: 2px dashed #555;"
        "  border-radius: 8px;"
        "  padding: 20px;"
        "  color: #888;"
        "  font-size: 13px;"
        "}");
    layout->addWidget(dropZone_);

    // Upload controls
    auto *uploadLayout = new QHBoxLayout;
    uploadBtn_ = new QPushButton(tr("Upload File..."));
    uploadBtn_->setStyleSheet(
        "QPushButton { background: #6c5ce7; color: white; border: none; border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover { background: #5b4bd5; }");
    uploadLayout->addWidget(uploadBtn_);
    uploadLayout->addStretch();
    layout->addLayout(uploadLayout);

    connect(uploadBtn_, &QPushButton::clicked, this, &PanoramaPage::onUploadClicked);

    // Media Library header
    auto *mlHeader = new QLabel(tr("Media Library"));
    QFont mlFont = mlHeader->font();
    mlFont.setPointSize(12);
    mlFont.setBold(true);
    mlHeader->setFont(mlFont);
    mlHeader->setStyleSheet("color: #fff;");
    layout->addWidget(mlHeader);

    // File list (files on device) - visual grid with thumbnails
    fileList_ = new QListWidget;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileList_->setMinimumHeight(200);
    fileList_->setMaximumHeight(320);
    fileList_->setViewMode(QListView::IconMode);
    fileList_->setIconSize(QSize(120, 80));
    fileList_->setGridSize(QSize(140, 120));
    fileList_->setResizeMode(QListView::Adjust);
    fileList_->setWrapping(true);
    fileList_->setWordWrap(true);
    fileList_->setSpacing(6);
    fileList_->setMovement(QListView::Static);
    fileList_->setStyleSheet(
        "QListWidget { background: #1e1e2e; border: 1px solid #444; border-radius: 6px; color: #ddd; padding: 6px; }"
        "QListWidget::item { background: #2a2a3a; border: 1px solid #3a3a4a; border-radius: 4px; padding: 4px; }"
        "QListWidget::item:selected { background: #6c5ce7; border: 1px solid #8b7cf7; }"
        "QListWidget::item:hover { background: #3a3a4e; }");
    layout->addWidget(fileList_);

    // Context menu on file list (replaces buttons)
    fileList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(fileList_, &QListWidget::customContextMenuRequested,
            this, &PanoramaPage::onFileListContextMenu);

    // Hidden buttons for backward compat (not shown in UI)
    setDisplayBtn_ = new QPushButton(scrollWidget);
    setDisplayBtn_->hide();
    deleteBtn_ = new QPushButton(scrollWidget);
    deleteBtn_->hide();

    // Refresh + Save row
    auto *actionLayout = new QHBoxLayout;
    refreshBtn_ = new QPushButton(tr("Refresh"));
    refreshBtn_->setStyleSheet(
        "QPushButton { background: #3d3d4d; color: #ddd; border: none; border-radius: 4px; padding: 6px 12px; }"
        "QPushButton:hover { background: #4d4d5d; }");
    connect(refreshBtn_, &QPushButton::clicked, this, &PanoramaPage::onRefreshClicked);
    actionLayout->addWidget(refreshBtn_);

    actionLayout->addStretch();

    customSaveBtn_ = new QPushButton(tr("Save"));
    customSaveBtn_->setMinimumHeight(36);
    customSaveBtn_->setMinimumWidth(120);
    customSaveBtn_->setStyleSheet(
        "QPushButton { background: #00b894; color: white; border: none; border-radius: 4px; padding: 8px 24px; font-weight: bold; font-size: 13px; }"
        "QPushButton:hover { background: #00a381; }");
    connect(customSaveBtn_, &QPushButton::clicked, this, &PanoramaPage::onCustomSave);
    actionLayout->addWidget(customSaveBtn_);

    layout->addLayout(actionLayout);

    layout->addStretch();

    scroll->setWidget(scrollWidget);

    auto *parentLayout = new QVBoxLayout(parent);
    parentLayout->setContentsMargins(0, 0, 0, 0);
    parentLayout->addWidget(scroll);
}

void PanoramaPage::setupDisplaySettings() {
    auto *settingsGroup = new QGroupBox(tr("Display Settings"));
    settingsGroup->setStyleSheet(
        "QGroupBox { border: 1px solid #444; border-radius: 6px; margin-top: 8px; padding-top: 16px; color: #fff; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 4px; }");

    auto *settingsLayout = new QHBoxLayout(settingsGroup);
    settingsLayout->setSpacing(20);

    // Brightness
    settingsLayout->addWidget(new QLabel(tr("Brightness:")));
    brightnessSlider_ = new QSlider(Qt::Horizontal);
    brightnessSlider_->setObjectName(
        QStringLiteral("displayBrightnessSlider"));
    brightnessSlider_->setRange(0, 100);
    brightnessSlider_->setValue(0);
    brightnessSlider_->setMaximumWidth(200);
    settingsLayout->addWidget(brightnessSlider_);
    brightnessLabel_ = new QLabel(QStringLiteral("--"));
    brightnessLabel_->setMinimumWidth(30);
    settingsLayout->addWidget(brightnessLabel_);

    connect(brightnessSlider_, &QSlider::valueChanged, this,
            [this](int val) { brightnessLabel_->setText(QString::number(val)); });
    connect(brightnessSlider_, &QSlider::sliderReleased, this,
            [this]() { onBrightnessChanged(brightnessSlider_->value()); });

    cbDisplayOff_ = new QCheckBox(tr("Display Off"));
    cbDisplayOff_->setObjectName(
        QStringLiteral("displayOffCheckBox"));
    cbDisplayOff_->setToolTip(
        tr("Disable only the PASE display backlight"));
    cbDisplayOff_->setStyleSheet("color: #ccc;");
    settingsLayout->addWidget(cbDisplayOff_);

    // Mirror mode
    cbMirrorMode_ = new QCheckBox(tr("Mirror Mode"));
    cbMirrorMode_->setObjectName(
        QStringLiteral("displayMirrorCheckBox"));
    cbMirrorMode_->setStyleSheet("color: #ccc;");
    settingsLayout->addWidget(cbMirrorMode_);

    cbWaterfallMode_ = new QCheckBox(tr("Waterfall Mode"));
    cbWaterfallMode_->setObjectName(
        QStringLiteral("displayWaterfallCheckBox"));
    cbWaterfallMode_->setToolTip(
        tr("Rotate the user interface by 90 degrees"));
    cbWaterfallMode_->setStyleSheet("color: #ccc;");
    settingsLayout->addWidget(cbWaterfallMode_);

    connect(cbDisplayOff_, &QCheckBox::clicked, this,
            [this](bool checked) {
                TryxRuntimeDisplayMutation mutation;
                mutation.backlightPresent = true;
                mutation.backlightEnabled = !checked;
                submitDisplayMutation(mutation);
            });
    const auto submitOrientation = [this]() {
        TryxRuntimeDisplayMutation mutation;
        mutation.orientationPresent = true;
        mutation.mirrorMode = cbMirrorMode_->isChecked();
        mutation.waterfallMode = cbWaterfallMode_->isChecked();
        submitDisplayMutation(mutation);
    };
    connect(cbMirrorMode_, &QCheckBox::clicked, this,
            [submitOrientation](bool) { submitOrientation(); });
    connect(cbWaterfallMode_, &QCheckBox::clicked, this,
            [this, submitOrientation](bool checked) {
                QSettings settings(
                    QStringLiteral("tryx-panorama"),
                    QStringLiteral("PanoramaPage"));
                if (checked &&
                    !settings.value(
                         QStringLiteral(
                             "display/waterfallWarningAccepted"),
                         false)
                         .toBool()) {
                    const QMessageBox::StandardButton response =
                        QMessageBox::warning(
                            this, tr("Waterfall Mode"),
                            tr("Waterfall Mode rotates the PASE interface and media by 90 degrees. Continue?"),
                            QMessageBox::Ok | QMessageBox::Cancel,
                            QMessageBox::Cancel);
                    if (response != QMessageBox::Ok) {
                        const QSignalBlocker blocker(cbWaterfallMode_);
                        cbWaterfallMode_->setChecked(false);
                        return;
                    }
                    settings.setValue(
                        QStringLiteral(
                            "display/waterfallWarningAccepted"),
                        true);
                }
                submitOrientation();
            });

    settingsLayout->addStretch();

    // Add to main layout
    auto *mainLayout = qobject_cast<QVBoxLayout *>(layout());
    if (mainLayout) {
        mainLayout->addWidget(settingsGroup);
    }
}

void PanoramaPage::loadBuiltinMedia() {
    presetTiles_.clear();
    QString mediaDir = builtinMediaDir();
    if (mediaDir.isEmpty()) return;

    QDir().mkpath(THUMB_CACHE_DIR);

    QDir dir(mediaDir);
    QStringList filters = {"*.mp4", "*.webm", "*.mkv", "*.avi", "*.mov",
                           "*.gif", "*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp"};
    auto entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);

    for (const auto &entry : entries) {
        MediaEntry me;
        me.filePath = entry.absoluteFilePath();
        me.fileName = entry.completeBaseName();
        me.format = entry.suffix().toUpper();
        me.sizeBytes = entry.size();

        auto *tile = new MediaTile(me, presetGridWidget_);
        connect(tile, &MediaTile::clicked, this, &PanoramaPage::onTileClicked);

        QString thumbName = entry.fileName().replace(' ', '_') + ".jpg";
        QString thumbPath = THUMB_CACHE_DIR + "/" + thumbName;
        QPixmap thumb = extractThumbnail(entry.absoluteFilePath(), thumbPath);
        tile->setThumbnail(thumb);

        // Add preset/upload badge overlay
        QString baseName = entry.completeBaseName();
        bool isPreset = !presetIdForName(baseName).isEmpty();
        auto *badge = new QLabel(isPreset ? tr("PRESET") : tr("UPLOAD"), tile);
        badge->setStyleSheet(isPreset
            ? "background: #00b894; color: white; padding: 2px 6px; border-radius: 3px; font-size: 9px; font-weight: bold;"
            : "background: #fdcb6e; color: #2d3436; padding: 2px 6px; border-radius: 3px; font-size: 9px; font-weight: bold;");
        badge->move(4, 4);
        badge->raise();

        presetTiles_.append(tile);
    }

    rebuildPresetGrid();
}

void PanoramaPage::onTileClicked(MediaTile *tile) {
    // Single selection for preset
    if (selectedPresetTile_ && selectedPresetTile_ != tile) {
        selectedPresetTile_->setSelected(false);
    }
    tile->setSelected(!tile->isSelected());
    selectedPresetTile_ = tile->isSelected() ? tile : nullptr;

    // Preview
    if (previewPlayer_) {
        previewPlayer_->stop();
    }
    previewLabel_->hide();

    if (selectedPresetTile_) {
        QString path = selectedPresetTile_->filePath();
        QString ext = path.section('.', -1).toLower();
        if (ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "avi" || ext == "mov") {
            ensurePreviewPlayer();
            previewLabel_->show();
            previewPlayer_->setSource(QUrl::fromLocalFile(path));
            previewPlayer_->setLoops(QMediaPlayer::Infinite);
            previewPlayer_->play();
        } else {
            QPixmap thumb = selectedPresetTile_->thumbnail();
            if (!thumb.isNull()) {
                previewLabel_->setPixmap(QPixmap::fromImage(
                    thumb.toImage().scaled(previewLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                previewLabel_->show();
            }
        }
    }
}

void PanoramaPage::ensurePreviewPlayer() {
    if (previewPlayer_) {
        return;
    }

    previewPlayer_ = new QMediaPlayer(this);
    previewSink_ = new QVideoSink(this);
    previewPlayer_->setVideoOutput(previewSink_);
    connect(previewSink_, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                QVideoFrame mappedFrame = frame;
                if (!mappedFrame.map(QVideoFrame::ReadOnly)) {
                    return;
                }
                const QImage image = mappedFrame.toImage();
                mappedFrame.unmap();
                if (!image.isNull()) {
                    previewLabel_->setPixmap(QPixmap::fromImage(
                        image.scaled(previewLabel_->size(),
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation)));
                }
            });
}

void PanoramaPage::onMetricToggled() {
    int count = 0;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) count++;
    }

    selectionCountLabel_->setText(tr("Selected: %1 / 3").arg(count));

    for (auto &opt : metricOptions_) {
        if (!opt.checkbox->isChecked()) {
            const bool sensorAvailable =
                !deviceMgr_->isPrinterClassDevicePresent() ||
                availablePrinterMetrics_.isEmpty() ||
                availablePrinterMetrics_.contains(opt.label);
            opt.checkbox->setEnabled(count < 3 && sensorAvailable);
        }
    }
}

void PanoramaPage::onChooseTextColor() {
    QColor color = QColorDialog::getColor(textColor_, this, tr("Text Color"));
    if (color.isValid()) {
        textColor_ = color;
        textColorBtn_->setStyleSheet(
            QString("background-color: %1; color: %2; padding: 4px 12px;")
                .arg(color.name())
                .arg(color.lightness() > 128 ? "#000" : "#fff"));
    }
}

TryxRuntimeApplyRequest PanoramaPage::fullScreenApplyRequest(
    const QStringList &media, const QString &ratio,
    const QString &playMode, const QStringList &metrics,
    const QString &presetId, bool replaceOverlay) const {
    TryxRuntimeApplyRequest request;
    request.media = media;
    request.ratio = ratio;
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = playMode;
    request.presetId = presetId;
    request.replaceOverlay = replaceOverlay;
    request.waterfallMode =
        cbWaterfallMode_ && cbWaterfallMode_->isChecked();
    if (replaceOverlay) {
        request.sysinfoLabels = metrics;
        request.settingsPosition = positionCombo_
            ? positionCombo_->currentData().toString()
            : QStringLiteral("Top");
        request.settingsAlign = alignCombo_
            ? alignCombo_->currentData().toString()
            : QStringLiteral("Left");
        request.settingsColor = textColor_.name();
        if (cbCpuBadge_ && cbCpuBadge_->isChecked()) {
            request.settingsBadges.append(
                QStringLiteral("CPU Badge"));
        }
        if (cbGpuBadge_ && cbGpuBadge_->isChecked()) {
            request.settingsBadges.append(
                QStringLiteral("GPU Badge"));
        }
    }
    return request;
}

QString PanoramaPage::startPrinterApply(
    const QStringList &media, const QString &ratio,
    const QString &playMode, const QStringList &metrics,
    const QString &presetId, bool updateMetrics) {
    if (!deviceMgr_->isPrinterDisplaySessionActive()) {
        emit statusMessage(tr(
            "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
        return {};
    }
    const TryxRuntimeApplyRequest request =
        fullScreenApplyRequest(media, ratio, playMode, metrics,
                               presetId, updateMetrics);
    const QString operationId =
        deviceMgr_->queueApplyOperation(QString(), request, updateMetrics);
    const TryxRuntimeOperationInfo operation =
        deviceMgr_->operationInfo(operationId);
    const bool accepted = operation.state != QStringLiteral("Failed") &&
                          operation.state != QStringLiteral("Cancelled");
    if (updateMetrics && accepted) {
        printerMetricsOperationId_ = operationId;
        pendingPrinterMetrics_ = metrics;
    }
    syncOperationPanel(deviceMgr_->operationSnapshot());
    return operationId;
}

void PanoramaPage::applyScreenConfig() {
    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }

    QStringList badges;
    if (cbCpuBadge_->isChecked()) badges << "CPU Badge";
    if (cbGpuBadge_->isChecked()) badges << "GPU Badge";

    // Get selected media from preset tile, using device preset ID if available
    QStringList media;
    QString presetId;
    if (selectedPresetTile_) {
        QFileInfo fi(selectedPresetTile_->filePath());
        QString baseName = fi.completeBaseName();
        presetId = presetIdForName(baseName);
        if (presetId.isEmpty()) {
            if (deviceMgr_->isPrinterClassDevicePresent()) {
                const QString localPath = selectedPresetTile_->filePath();
                const TryxRuntimeApplyRequest request =
                    fullScreenApplyRequest(
                        {},
                        ratioCombo_
                            ? ratioCombo_->currentText()
                            : QStringLiteral("2:1"),
                        QStringLiteral("Single"), labels, QString(),
                        true);
                activeOperationId_ =
                    deviceMgr_->queueEnsureMediaAndApplyOperation(
                        QString(), localPath, request);
                const TryxRuntimeOperationInfo operation =
                    deviceMgr_->operationInfo(activeOperationId_);
                if (operation.state != QStringLiteral("Failed") &&
                    operation.state != QStringLiteral("Cancelled")) {
                    printerMetricsOperationId_ = activeOperationId_;
                    pendingPrinterMetrics_ = labels;
                    uploadSourcePaths_.insert(activeOperationId_, localPath);
                    emit statusMessage(
                        tr("Checking and applying the selected media..."));
                }
                syncOperationPanel(deviceMgr_->operationSnapshot());
                return;
            }

            // Non-preset file: upload to device via ADB first, then set config
            QString localPath = selectedPresetTile_->filePath();
            QString remoteName = fi.fileName();
            media << remoteName;

            fprintf(stderr, "[panorama] uploading '%s' to device...\n",
                    remoteName.toStdString().c_str());
            emit statusMessage(tr("Uploading %1...").arg(remoteName));

            // Upload in background, set config after upload completes
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(deviceMgr_, &DeviceManager::mediaUploaded, this,
                [this, labels, badges, conn](const QString &uploadedName) {
                    disconnect(*conn);
                    // Use actual uploaded filename (may be converted to .mp4)
                    QStringList actualMedia;
                    actualMedia << uploadedName;
                    fprintf(stderr, "[panorama] upload done: '%s', setting screen config\n",
                            uploadedName.toStdString().c_str());
                    activeLegacyMetrics_ = labels;
                    legacyMetricsStartPending_ = !labels.isEmpty();
                    deviceMgr_->setScreenConfig(
                        actualMedia,
                        ratioCombo_ ? ratioCombo_->currentText() : "2:1",
                        "Full Screen", "Single", labels,
                        positionCombo_->currentData().toString(),
                        textColor_.name(),
                        alignCombo_->currentData().toString(),
                        badges, 0, QString()
                    );
                    emit statusMessage(tr("Configuration applied"));
                });

            auto errConn = std::make_shared<QMetaObject::Connection>();
            *errConn = connect(deviceMgr_, &DeviceManager::deviceError, this,
                [this, conn, errConn](const QString &msg) {
                    disconnect(*conn);
                    disconnect(*errConn);
                    fprintf(stderr, "[panorama] upload failed: %s\n",
                            msg.toStdString().c_str());
                    emit statusMessage(tr("Upload failed: %1").arg(msg));
                });

            deviceMgr_->uploadMedia(localPath);
            return;  // Config will be set after upload completes
        }
    }

    fprintf(stderr, "[panorama] save: preset='%s' media=%lld metrics=%lld\n",
            presetId.toStdString().c_str(), (long long)media.size(), (long long)labels.size());

    if (deviceMgr_->isPrinterClassDevicePresent()) {
        if (!deviceMgr_->isPrinterDisplaySessionActive()) {
            emit statusMessage(tr(
                "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
            return;
        }
        if (presetId.isEmpty() && media.isEmpty()) {
            const TryxRuntimeApplyRequest request =
                fullScreenApplyRequest(
                    {},
                    ratioCombo_
                        ? ratioCombo_->currentText()
                        : QStringLiteral("2:1"),
                    QStringLiteral("Single"), labels, QString(),
                    true);
            const QString operationId =
                deviceMgr_->queueApplyOperation(QString(), request, true);
            const TryxRuntimeOperationInfo operation =
                deviceMgr_->operationInfo(operationId);
            if (operation.state != QStringLiteral("Failed") &&
                operation.state != QStringLiteral("Cancelled")) {
                printerMetricsOperationId_ = operationId;
                pendingPrinterMetrics_ = labels;
            }
            syncOperationPanel(deviceMgr_->operationSnapshot());
            return;
        }
        startPrinterApply(media,
                          ratioCombo_ ? ratioCombo_->currentText()
                                      : QStringLiteral("2:1"),
                          QStringLiteral("Single"), labels, presetId);
    } else {
        activeLegacyMetrics_ = labels;
        legacyMetricsStartPending_ = !labels.isEmpty();
        deviceMgr_->setScreenConfig(
            media,
            ratioCombo_ ? ratioCombo_->currentText() : "2:1",
            "Full Screen",
            "Single",
            labels,
            positionCombo_->currentData().toString(),
            textColor_.name(),
            alignCombo_->currentData().toString(),
            badges,
            0,
            presetId
        );
    }
}

void PanoramaPage::setUploadBusy(bool busy) {
    uploadBusy_ = busy;
    updateActionAvailability();
    if (!busy) {
        progressBar_->setRange(0, 0);
    }
}

void PanoramaPage::updateActionAvailability() {
    const bool printerClass = deviceMgr_->isPrinterClassDevicePresent();
    const bool sessionReady =
        printerClass
            ? deviceMgr_->isPrinterDisplaySessionActive()
            : deviceMgr_->isConnected();
    const bool actionsEnabled = !uploadBusy_ && sessionReady;
    const bool displayStateValid =
        !printerClass || deviceMgr_->displayState().valid;

    uploadBtn_->setEnabled(actionsEnabled);
    refreshBtn_->setEnabled(actionsEnabled && !refreshPending_);
    setDisplayBtn_->setEnabled(actionsEnabled);
    customSaveBtn_->setEnabled(actionsEnabled);
    if (presetSaveBtn_) {
        presetSaveBtn_->setEnabled(actionsEnabled);
    }
    retryBtn_->setEnabled(actionsEnabled && !retryOperationId_.isEmpty());
    cancelBtn_->setEnabled(uploadBusy_ && !activeOperationId_.isEmpty());
    brightnessSlider_->setEnabled(
        sessionReady && displayStateValid &&
        (actionsEnabled || !brightnessOperationId_.isEmpty()));
    cbDisplayOff_->setEnabled(
        printerClass && actionsEnabled &&
        deviceMgr_->displayState().valid &&
        displayMutationReady_);
    cbMirrorMode_->setEnabled(
        printerClass && actionsEnabled &&
        deviceMgr_->displayState().valid &&
        displayMutationReady_);
    cbWaterfallMode_->setEnabled(
        printerClass && actionsEnabled &&
        deviceMgr_->displayState().valid &&
        displayMutationReady_);
    fullScreenRadio_->setEnabled(actionsEnabled);
    splitConfigWidget_->setEnabled(actionsEnabled);

    if (printerClass) {
        splitScreenRadio_->setEnabled(actionsEnabled);
        fileList_->setSelectionMode(
            splitScreenRadio_->isChecked()
                ? QAbstractItemView::ExtendedSelection
                : QAbstractItemView::SingleSelection);
        const int ratioIndex = ratioCombo_->findText(QStringLiteral("2:1"));
        if (ratioIndex >= 0) {
            ratioCombo_->setCurrentIndex(ratioIndex);
        }
        ratioCombo_->setEnabled(false);
        playModeCombo_->setEnabled(actionsEnabled);
        splitConfigWidget_->setPaseMode(true);
    } else {
        splitScreenRadio_->setEnabled(actionsEnabled);
        fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        ratioCombo_->setEnabled(actionsEnabled);
        playModeCombo_->setEnabled(actionsEnabled);
        splitConfigWidget_->setPaseMode(false);
    }
}

QStringList PanoramaPage::selectedDeviceMediaNames() const {
    QStringList names;
    for (const QListWidgetItem *item : fileList_->selectedItems()) {
        QString name = item->data(Qt::UserRole).toString();
        if (name.isEmpty()) {
            name = item->text().section(QLatin1Char('\n'), 0, 0);
        }
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

void PanoramaPage::updateCustomMetricsButton() {
    int count = 0;
    for (const QCheckBox *checkbox : customMetricCheckboxes_) {
        if (checkbox->isChecked()) {
            ++count;
        }
    }
    customMetricsBtn_->setText(
        QString::fromUtf8("%1 / 3 \u25BC").arg(count));
}

QString PanoramaPage::operationStatusText(
    const TryxRuntimeOperationInfo &info) const {
    QString title = info.subject.trimmed();
    if (title.isEmpty()) {
        title = info.kind.trimmed();
    }
    QString detail = info.message.trimmed();
    if (detail.isEmpty()) {
        detail = info.stage.trimmed();
    }

    QString text;
    if (!title.isEmpty() && !detail.isEmpty()) {
        text = tr("%1: %2").arg(title, detail);
    } else {
        text = title.isEmpty() ? detail : title;
    }
    if (!info.primaryErrorMessage.trimmed().isEmpty() &&
        (info.state == QStringLiteral("RetryAvailable") ||
         info.stage == QStringLiteral("RecoveringFinalization"))) {
        const QString primaryError =
            tr("Initial transfer error: %1")
                .arg(info.primaryErrorMessage.trimmed());
        text = text.isEmpty()
            ? primaryError
            : text + QLatin1Char('\n') + primaryError;
    }
    if (info.total > 0) {
        const bool showingPreviousAttempt =
            info.state == QStringLiteral("RetryAvailable") &&
            info.confirmedBytes > 0;
        const qint64 visibleCompleted = showingPreviousAttempt
            ? info.confirmedBytes
            : info.completed;
        const QString progress = showingPreviousAttempt
            ? tr("Confirmed in the previous attempt: %1 of %2 bytes")
                  .arg(qMax<qint64>(0, visibleCompleted))
                  .arg(info.total)
            : tr("%1 of %2 bytes")
                  .arg(qMax<qint64>(0, visibleCompleted))
                  .arg(info.total);
        text = text.isEmpty() ? progress : text + QLatin1Char('\n') + progress;
    }
    if (info.state == QStringLiteral("RetryAvailable") &&
        info.lastConfirmedChunkIndex >= 0) {
        const QString confirmedChunk =
            tr("Last confirmed chunk: %1")
                .arg(info.lastConfirmedChunkIndex + 1);
        text = text.isEmpty()
            ? confirmedChunk
            : text + QLatin1Char('\n') + confirmedChunk;
    }
    return text;
}

void PanoramaPage::syncOperationPanel(
    const TryxRuntimeOperationsSnapshot &snapshot) {
    TryxRuntimeOperationInfo active;
    QList<TryxRuntimeOperationInfo> retryCandidates;
    TryxRuntimeOperationInfo deleteReconciliation;
    for (const TryxRuntimeOperationInfo &operation : snapshot.operations) {
        if (operation.id == snapshot.activeOperationId) {
            active = operation;
        }
        if (operation.state == QStringLiteral("RetryAvailable") &&
            operation.retryMode == QStringLiteral("PreparedMedia")) {
            retryCandidates.append(operation);
        }
        if (operation.state == QStringLiteral("RetryAvailable") &&
            operation.retryMode == QStringLiteral("DeleteReconcile")) {
            deleteReconciliation = operation;
        }
    }

    const auto showProgress = [this](const TryxRuntimeOperationInfo &operation,
                                     bool indeterminateWhenEmpty) {
        if (operation.total > 0) {
            progressBar_->setRange(0, 100);
            const qint64 visibleCompleted =
                operation.state == QStringLiteral("RetryAvailable") &&
                    operation.confirmedBytes > 0
                    ? operation.confirmedBytes
                    : operation.completed;
            const int percent = static_cast<int>(qBound<qint64>(
                qint64(0),
                (qMax<qint64>(qint64(0), visibleCompleted) * 100) /
                       operation.total,
                qint64(100)));
            progressBar_->setValue(percent);
            progressBar_->show();
        } else if (indeterminateWhenEmpty) {
            progressBar_->setRange(0, 0);
            progressBar_->show();
        } else {
            progressBar_->hide();
        }
    };

    if (!active.id.isEmpty()) {
        displayMutationReady_ = false;
        activeOperationId_ = active.id;
        retryOperationId_ = retryCandidates.size() == 1
            ? retryCandidates.constFirst().id
            : QString();
        setUploadBusy(true);
        operationStatusLabel_->setText(operationStatusText(active));
        showProgress(active, true);
        retryBtn_->hide();
        cancelBtn_->show();
        operationPanel_->show();
        return;
    }

    activeOperationId_.clear();
    setUploadBusy(false);
    cancelBtn_->hide();

    if (retryCandidates.size() == 1) {
        const TryxRuntimeOperationInfo &retry = retryCandidates.constFirst();
        retryOperationId_ = retry.id;
        updateActionAvailability();
        operationStatusLabel_->setText(operationStatusText(retry));
        showProgress(retry, false);
        retryBtn_->show();
        operationPanel_->show();
        return;
    }

    retryOperationId_.clear();
    updateActionAvailability();
    retryBtn_->hide();
    if (retryCandidates.size() > 1) {
        operationStatusLabel_->setText(tr(
            "Multiple prepared uploads require reconciliation; restart the background runtime before retrying"));
        progressBar_->hide();
        operationPanel_->show();
        return;
    }

    if (!deleteReconciliation.id.isEmpty()) {
        operationStatusLabel_->setText(
            operationStatusText(deleteReconciliation));
        progressBar_->hide();
        operationPanel_->show();
        return;
    }

    if (!snapshot.operations.isEmpty()) {
        const TryxRuntimeOperationInfo &last = snapshot.operations.constLast();
        if (last.state == QStringLiteral("Failed")) {
            operationStatusLabel_->setText(operationStatusText(last));
            showProgress(last, false);
            operationPanel_->show();
            return;
        }
    }

    operationStatusLabel_->clear();
    progressBar_->hide();
    operationPanel_->hide();
}

void PanoramaPage::savePageState() {
    QSettings settings("tryx-panorama", "PanoramaPage");

    // Save selected preset tile name
    if (selectedPresetTile_) {
        QFileInfo fi(selectedPresetTile_->filePath());
        settings.setValue("preset/selectedName", fi.completeBaseName());
    } else {
        settings.remove("preset/selectedName");
    }

    // Save checked metrics
    QStringList checkedMetrics;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            checkedMetrics << opt.label;
        }
    }
    settings.setValue("metrics/checked", checkedMetrics);

    // Save display settings
    settings.setValue("display/position", positionCombo_->currentData().toString());
    settings.setValue("display/align", alignCombo_->currentData().toString());
    settings.setValue("display/textColor", textColor_.name());
    settings.setValue("display/cpuBadge", cbCpuBadge_->isChecked());
    settings.setValue("display/gpuBadge", cbGpuBadge_->isChecked());
    settings.remove("display/sleepMode");
    settings.setValue("display/displayOff",
                      cbDisplayOff_->isChecked());
    settings.setValue("display/mirrorMode", cbMirrorMode_->isChecked());
    settings.setValue("display/waterfallMode",
                      cbWaterfallMode_->isChecked());
}

void PanoramaPage::restorePageState() {
    QSettings settings("tryx-panorama", "PanoramaPage");

    // Restore selected preset tile
    QString savedName = settings.value("preset/selectedName").toString();
    if (!savedName.isEmpty()) {
        for (auto *tile : presetTiles_) {
            QFileInfo fi(tile->filePath());
            if (fi.completeBaseName() == savedName) {
                tile->setSelected(true);
                selectedPresetTile_ = tile;
                break;
            }
        }
    }

    // Restore checked metrics
    QStringList checkedMetrics = settings.value("metrics/checked").toStringList();
    if (!checkedMetrics.isEmpty()) {
        for (auto &opt : metricOptions_) {
            opt.checkbox->setChecked(checkedMetrics.contains(opt.label));
        }
        // Trigger count update
        onMetricToggled();
    }

    // Restore display settings
    if (settings.contains("display/position")) {
        int idx = positionCombo_->findData(settings.value("display/position").toString());
        if (idx >= 0) positionCombo_->setCurrentIndex(idx);
    }
    if (settings.contains("display/align")) {
        int idx = alignCombo_->findData(settings.value("display/align").toString());
        if (idx >= 0) alignCombo_->setCurrentIndex(idx);
    }
    if (settings.contains("display/textColor")) {
        const QColor savedColor(
            settings.value("display/textColor").toString());
        textColor_ = savedColor.isValid()
            ? savedColor
            : QColor(QStringLiteral("#DCDCDC"));
        textColorBtn_->setStyleSheet(
            QString("background-color: %1; color: %2; padding: 4px 12px;")
                .arg(textColor_.name())
                .arg(textColor_.lightness() > 128 ? "#000" : "#fff"));
    }
    if (settings.contains("display/cpuBadge")) {
        cbCpuBadge_->setChecked(settings.value("display/cpuBadge").toBool());
    }
    if (settings.contains("display/gpuBadge")) {
        cbGpuBadge_->setChecked(settings.value("display/gpuBadge").toBool());
    }
    if (settings.contains("display/displayOff")) {
        cbDisplayOff_->setChecked(
            settings.value("display/displayOff").toBool());
    }
    if (settings.contains("display/mirrorMode")) {
        cbMirrorMode_->setChecked(
            settings.value("display/mirrorMode").toBool());
    }
    if (settings.contains("display/waterfallMode")) {
        cbWaterfallMode_->setChecked(
            settings.value("display/waterfallMode").toBool());
    }
}

void PanoramaPage::onPresetSave() {
    applyScreenConfig();
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        savePageState();
        return;
    }

    // Auto-start metrics sending after Save (like KANALI)
    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }

    // Persist current page state
    savePageState();

    if (labels.isEmpty() || !deviceMgr_->isPrinterClassDevicePresent()) {
        emit statusMessage(tr("Configuration applied"));
    }
}

void PanoramaPage::startMetrics() {
    QStringList labels;
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        labels = activePrinterMetrics_;
    } else if (!activeLegacyMetrics_.isEmpty()) {
        labels = activeLegacyMetrics_;
    } else {
        for (const auto &opt : metricOptions_) {
            if (opt.checkbox->isChecked()) {
                labels << opt.label;
            }
        }
    }
    if (labels.isEmpty()) return;

    if (deviceMgr_->isPrinterClassDevicePresent()) {
        metricsTimer_->stop();
        const bool stateChanged = !metricsRunning_;
        metricsRunning_ = true;
        if (stateChanged) {
            emit metricsRunningChanged(true);
        }
        metricsStatusLabel_->setText(
            tr("Metrics active in background runtime"));
        metricsStatusLabel_->setStyleSheet("color: #00b894;");
        return;
    }

    if (metricsRunning_) {
        metricsTimer_->setInterval(2000);
        metricsStatusLabel_->setText(tr("Metrics active"));
        return;
    }

    metricsRunning_ = true;
    metricsTimer_->start(2000);
    emit metricsRunningChanged(true);
    metricsStatusLabel_->setText(tr("Metrics active"));
    metricsStatusLabel_->setStyleSheet("color: #00b894;");

    onSendMetrics();
}

void PanoramaPage::stopMetrics() {
    if (!metricsRunning_) return;

    if (deviceMgr_->isPrinterClassDevicePresent() &&
        !activePrinterMetrics_.isEmpty()) {
        emit statusMessage(tr(
            "PASE metrics are controlled by the saved display configuration"));
        return;
    }

    metricsRunning_ = false;
    metricsTimer_->stop();
    emit metricsRunningChanged(false);
    metricsStatusLabel_->setText("");
    metricsStatusLabel_->setStyleSheet("color: #888;");
}

void PanoramaPage::onSendMetrics() {
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        return;
    }
    monitor_->update();
    const auto metrics = monitor_->currentMetrics();

    QStringList labels, values, units;

    const auto appendMetric = [&labels, &values, &units](
                                  const QString &label,
                                  double value,
                                  const QString &unit,
                                  bool available) {
        if (!available) {
            return;
        }
        labels << label;
        values << QString::number(value, 'f', 0);
        units << unit;
    };

    appendMetric(QStringLiteral("CPU Temperature"),
                 metrics.cpu.temperature, QStringLiteral("°C"),
                 metrics.cpu.temperatureAvailable);
    appendMetric(QStringLiteral("CPU Usage"), metrics.cpu.usagePercent,
                 QStringLiteral("%"), metrics.cpu.usageAvailable);
    appendMetric(QStringLiteral("CPU Frequency"),
                 metrics.cpu.frequencyMHz, QStringLiteral("MHZ"),
                 metrics.cpu.frequencyAvailable);

    if (!metrics.gpus.isEmpty()) {
        const GpuMetrics &gpu = metrics.gpus.first();
        appendMetric(QStringLiteral("GPU Temperature"), gpu.temperature,
                     QStringLiteral("°C"), gpu.temperatureAvailable);
        appendMetric(QStringLiteral("GPU Usage"), gpu.usagePercent,
                     QStringLiteral("%"), gpu.usageAvailable);
        appendMetric(QStringLiteral("GPU Frequency"), gpu.frequencyMHz,
                     QStringLiteral("MHZ"), gpu.frequencyAvailable);
    }

    appendMetric(QStringLiteral("Memory Usage"), metrics.ram.usagePercent,
                 QStringLiteral("%"), metrics.ram.usageAvailable);

    const QStringList selected = deviceMgr_->isPrinterClassDevicePresent()
        ? activePrinterMetrics_
        : !activeLegacyMetrics_.isEmpty()
            ? activeLegacyMetrics_
            : [&]() {
              QStringList result;
              for (const auto &option : metricOptions_) {
                  if (option.checkbox->isChecked()) {
                      result << option.label;
                  }
              }
              return result;
          }();
    for (const QString &selectedLabel : selected) {
        const int idx = labels.indexOf(selectedLabel);
            if (idx >= 0) {
                fprintf(stderr, "[sysinfo] %s = %s %s\n",
                        labels[idx].toStdString().c_str(),
                        values[idx].toStdString().c_str(),
                        units[idx].toStdString().c_str());
            }
    }

    deviceMgr_->sendSysinfo(labels, values, units);
    metricsStatusLabel_->setText(tr("Metrics active"));
}

// Customization tab slots

void PanoramaPage::onUploadClicked() {
    if (deviceMgr_->isPrinterClassDevicePresent() &&
        !deviceMgr_->isPrinterDisplaySessionActive()) {
        emit statusMessage(tr(
            "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
        return;
    }
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select media file"), QString(),
        "Media (*.mp4 *.webm *.mkv *.avi *.mov *.gif *.jpg *.jpeg *.png *.bmp *.webp)");

    if (!path.isEmpty()) {
        pendingUploadSourcePath_ = path;
        if (deviceMgr_->isPrinterClassDevicePresent()) {
            activeOperationId_ =
                deviceMgr_->queueUploadOperation(QString(), path, false);
            const TryxRuntimeOperationInfo operation =
                deviceMgr_->operationInfo(activeOperationId_);
            if (operation.state != QStringLiteral("Failed") &&
                operation.state != QStringLiteral("Cancelled")) {
                uploadSourcePaths_.insert(activeOperationId_, path);
            }
            syncOperationPanel(deviceMgr_->operationSnapshot());
        } else {
            setUploadBusy(true);
            deviceMgr_->uploadMedia(path);
        }
    }
}

void PanoramaPage::onUploadBuiltinClicked() {
    // Upload selected preset tile to device
    if (!selectedPresetTile_) {
        emit statusMessage(tr("Select a video from the library"));
        return;
    }

    pendingUploadSourcePath_ = selectedPresetTile_->filePath();
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        if (!deviceMgr_->isPrinterDisplaySessionActive()) {
            emit statusMessage(tr(
                "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
            return;
        }
        activeOperationId_ = deviceMgr_->queueUploadOperation(
            QString(), pendingUploadSourcePath_, false);
        const TryxRuntimeOperationInfo operation =
            deviceMgr_->operationInfo(activeOperationId_);
        if (operation.state != QStringLiteral("Failed") &&
            operation.state != QStringLiteral("Cancelled")) {
            uploadSourcePaths_.insert(activeOperationId_,
                                      pendingUploadSourcePath_);
        }
        syncOperationPanel(deviceMgr_->operationSnapshot());
    } else {
        setUploadBusy(true);
        deviceMgr_->uploadMedia(pendingUploadSourcePath_);
    }
}

void PanoramaPage::onScreenModeChanged() {
    bool isSplit = splitScreenRadio_->isChecked();
    fullScreenControls_->setVisible(!isSplit);
    splitConfigWidget_->setVisible(isSplit);
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        fileList_->setSelectionMode(
            isSplit ? QAbstractItemView::ExtendedSelection
                    : QAbstractItemView::SingleSelection);
        splitConfigWidget_->setPaseMode(true);
    }
}

void PanoramaPage::onCustomSave() {
    if (deviceMgr_->isPrinterClassDevicePresent() &&
        !deviceMgr_->isPrinterDisplaySessionActive()) {
        emit statusMessage(tr(
            "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
        return;
    }
    bool isSplit = splitScreenRadio_->isChecked();

    if (isSplit) {
        // Screen Splitting mode
        QStringList leftMedia = splitConfigWidget_->leftMedia();
        QStringList rightMedia = splitConfigWidget_->rightMedia();

        if (leftMedia.isEmpty() || rightMedia.isEmpty()) {
            emit statusMessage(tr("Assign media to both left and right sides"));
            return;
        }

        QStringList allMedia;
        allMedia << leftMedia << rightMedia;

        QStringList leftMetrics = splitConfigWidget_->leftMetrics();
        QStringList rightMetrics = splitConfigWidget_->rightMetrics();
        const QString playMode =
            deviceMgr_->isPrinterClassDevicePresent()
            ? QStringLiteral("Single")
            : splitConfigWidget_->playMode();
        const QStringList leftBadges =
            splitConfigWidget_->leftBadges();
        const QStringList rightBadges =
            splitConfigWidget_->rightBadges();

        fprintf(stderr, "[panorama] split save: left=%lld right=%lld leftMetrics=%lld rightMetrics=%lld\n",
                (long long)leftMedia.size(), (long long)rightMedia.size(),
                (long long)leftMetrics.size(), (long long)rightMetrics.size());

        if (deviceMgr_->isPrinterClassDevicePresent()) {
            TryxRuntimeApplyRequest request;
            request.media = allMedia;
            request.ratio = QStringLiteral("2:1");
            request.screenMode =
                QStringLiteral("Screen Splitting");
            request.playMode = QStringLiteral("Single");
            request.sysinfoLabels = leftMetrics;
            request.settingsBadges = leftBadges;
            request.settingsPosition =
                splitConfigWidget_->leftPosition();
            request.settingsColor =
                splitConfigWidget_->leftColor();
            request.settingsAlign =
                splitConfigWidget_->leftAlignment();
            request.sysinfoLabels2 = rightMetrics;
            request.settingsBadges2 = rightBadges;
            request.settingsPosition2 =
                splitConfigWidget_->rightPosition();
            request.settingsColor2 =
                splitConfigWidget_->rightColor();
            request.settingsAlign2 =
                splitConfigWidget_->rightAlignment();
            request.waterfallMode =
                cbWaterfallMode_->isChecked();
            request.replaceOverlay = true;
            activeOperationId_ =
                deviceMgr_->queueApplyOperation(
                    QString(), request, true);
            const TryxRuntimeOperationInfo operation =
                deviceMgr_->operationInfo(activeOperationId_);
            if (operation.state != QStringLiteral("Failed") &&
                operation.state != QStringLiteral("Cancelled")) {
                printerMetricsOperationId_ = activeOperationId_;
                pendingPrinterMetrics_ = leftMetrics;
                for (const QString &metric : rightMetrics) {
                    if (!pendingPrinterMetrics_.contains(metric)) {
                        pendingPrinterMetrics_.append(metric);
                    }
                }
            }
            syncOperationPanel(deviceMgr_->operationSnapshot());
        } else {
            activeLegacyMetrics_ = leftMetrics;
            for (const QString &metric : rightMetrics) {
                if (!activeLegacyMetrics_.contains(metric)) {
                    activeLegacyMetrics_.append(metric);
                }
            }
            legacyMetricsStartPending_ =
                !activeLegacyMetrics_.isEmpty();
            deviceMgr_->setScreenConfig(
                allMedia, QStringLiteral("2:1"),
                QStringLiteral("Screen Splitting"), playMode,
                leftMetrics, splitConfigWidget_->leftPosition(),
                splitConfigWidget_->leftColor(),
                splitConfigWidget_->leftAlignment(), leftBadges, 0,
                QString(), rightMetrics, rightBadges,
                cbWaterfallMode_->isChecked());
            emit statusMessage(
                tr("Screen Splitting configuration applied"));
        }
    } else {
        // Full Screen mode - use selected files from list
        const QStringList media = selectedDeviceMediaNames();

        QString ratio = ratioCombo_->currentText();
        QString playMode = playModeCombo_->currentData().toString();

        // Collect selected metrics
        QStringList metrics;
        for (auto *cb : customMetricCheckboxes_)
            if (cb->isChecked()) metrics << cb->property("protocolLabel").toString();

        if (deviceMgr_->isPrinterClassDevicePresent()) {
            if (media.isEmpty()) {
                const TryxRuntimeApplyRequest request =
                    fullScreenApplyRequest(
                        {}, ratio, playMode, metrics, QString(),
                        true);
                const QString operationId =
                    deviceMgr_->queueApplyOperation(
                        QString(), request, true);
                const TryxRuntimeOperationInfo operation =
                    deviceMgr_->operationInfo(operationId);
                if (operation.state != QStringLiteral("Failed") &&
                    operation.state != QStringLiteral("Cancelled")) {
                    printerMetricsOperationId_ = operationId;
                    pendingPrinterMetrics_ = metrics;
                }
                syncOperationPanel(deviceMgr_->operationSnapshot());
            } else {
                startPrinterApply(media, ratio, playMode, metrics);
            }
        } else {
            if (media.isEmpty()) {
                emit statusMessage(tr("Select files to display"));
                return;
            }
            activeLegacyMetrics_ = metrics;
            legacyMetricsStartPending_ = !metrics.isEmpty();
            deviceMgr_->setScreenConfig(media, ratio, "Full Screen", playMode,
                                        metrics, "Top", "#FFFFFF", "Left",
                                        {}, 0);
        }

        if (!deviceMgr_->isPrinterClassDevicePresent()) {
            emit statusMessage(tr("Full Screen configuration applied"));
        }
    }
}

void PanoramaPage::onFileListContextMenu(const QPoint &pos) {
    auto *item = fileList_->itemAt(pos);
    if (!item) return;

    QMenu menu(this);
    bool isSplit = splitScreenRadio_->isChecked();

    if (isSplit) {
        auto *setLeft = menu.addAction(tr("Set to left side"));
        auto *setRight = menu.addAction(tr("Set to right side"));

        connect(setLeft, &QAction::triggered, this, [this, item]() {
            QString filename = item->data(Qt::UserRole).toString();
            if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
            QPixmap thumb;
            const QString thumbPath =
                thumbnailCachePathForDeviceFile(filename);
            if (QFileInfo::exists(thumbPath)) {
                thumb = QPixmap(thumbPath);
            }
            splitConfigWidget_->assignToLeft(filename, thumb);
        });
        connect(setRight, &QAction::triggered, this, [this, item]() {
            QString filename = item->data(Qt::UserRole).toString();
            if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
            QPixmap thumb;
            const QString thumbPath =
                thumbnailCachePathForDeviceFile(filename);
            if (QFileInfo::exists(thumbPath)) {
                thumb = QPixmap(thumbPath);
            }
            splitConfigWidget_->assignToRight(filename, thumb);
        });
    } else {
        auto *setDisplay = menu.addAction(tr("Set as display"));
        connect(setDisplay, &QAction::triggered, this, [this, item]() {
            QString filename = item->data(Qt::UserRole).toString();
            if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
            QStringList media;
            media << filename;
            QString ratio = ratioCombo_->currentText();
            QString playMode = playModeCombo_->currentData().toString();
            if (deviceMgr_->isPrinterClassDevicePresent()) {
                startPrinterApply(media, ratio, playMode, {}, QString(),
                                  false);
            } else {
                deviceMgr_->setScreenConfig(media, ratio, "Full Screen",
                                            playMode);
                emit statusMessage(tr("Screen config applied"));
            }
        });
    }

    menu.addSeparator();
    auto *deleteAction = menu.addAction(tr("Delete"));
    const bool printerClass = deviceMgr_->isPrinterClassDevicePresent();
    const bool deleteAllowed = !printerClass ||
        item->data(MEDIA_DELETE_ALLOWED_ROLE).toBool();
    deleteAction->setEnabled(
        deleteAllowed && !uploadBusy_ &&
        (!printerClass || deviceMgr_->isPrinterDisplaySessionActive()));
    if (printerClass && !deleteAllowed) {
        deleteAction->setStatusTip(
            item->data(MEDIA_DELETE_BLOCK_REASON_ROLE).toString());
    }
    connect(deleteAction, &QAction::triggered, this, [this, item]() {
        deleteDeviceItems({item});
    });

    menu.exec(fileList_->mapToGlobal(pos));
}

void PanoramaPage::onSetDisplayClicked() {
    const QStringList media = selectedDeviceMediaNames();
    if (media.isEmpty()) {
        emit statusMessage(tr("Select files to display"));
        return;
    }
    QString ratio = ratioCombo_ ? ratioCombo_->currentText() : "2:1";
    QString screenMode = splitScreenRadio_->isChecked()
        ? QStringLiteral("Screen Splitting")
        : QStringLiteral("Full Screen");
    QString playMode = playModeCombo_ ? playModeCombo_->currentData().toString() : "Single";

    if (deviceMgr_->isPrinterClassDevicePresent()) {
        if (!deviceMgr_->isPrinterDisplaySessionActive()) {
            emit statusMessage(tr(
                "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
            return;
        }
        const int requiredMedia =
            screenMode == QStringLiteral("Screen Splitting") ? 2 : 1;
        if (media.size() != requiredMedia) {
            emit statusMessage(
                tr("Select %1 media file(s) for this screen mode")
                    .arg(requiredMedia));
            return;
        }
        TryxRuntimeApplyRequest request;
        request.media = media;
        request.ratio = ratio;
        request.screenMode = screenMode;
        request.playMode =
            screenMode == QStringLiteral("Screen Splitting")
            ? QStringLiteral("Single")
            : playMode;
        activeOperationId_ =
            deviceMgr_->queueApplyOperation(QString(), request);
        syncOperationPanel(deviceMgr_->operationSnapshot());
    } else {
        deviceMgr_->setScreenConfig(media, ratio, screenMode, playMode);
        emit statusMessage(tr("Screen config applied"));
    }
}

void PanoramaPage::onDeleteClicked() {
    const QList<QListWidgetItem *> selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to delete"));
        return;
    }
    deleteDeviceItems(selected);
}

void PanoramaPage::deleteDeviceItems(
    const QList<QListWidgetItem *> &items) {
    QStringList files;
    quint64 totalSize = 0;
    for (QListWidgetItem *item : items) {
        if (!item) {
            continue;
        }
        QString filename = item->data(Qt::UserRole).toString();
        if (filename.isEmpty()) {
            filename = item->text().section(QLatin1Char('\n'), 0, 0);
        }
        if (deviceMgr_->isPrinterClassDevicePresent() &&
            !item->data(MEDIA_DELETE_ALLOWED_ROLE).toBool()) {
            const QString reason =
                item->data(MEDIA_DELETE_BLOCK_REASON_ROLE).toString();
            emit statusMessage(reason.isEmpty()
                                   ? tr("This PASE media file cannot be deleted")
                                   : tr("This PASE media file cannot be deleted: %1")
                                         .arg(reason));
            return;
        }
        files << filename;
        totalSize += item->data(MEDIA_SIZE_ROLE).toULongLong();
    }
    if (files.isEmpty()) {
        return;
    }

    const QString confirmation =
        deviceMgr_->isPrinterClassDevicePresent()
            ? (files.size() == 1
                   ? tr("Delete this file from PASE?\n\nName: %1\nSize: %2 bytes\n\nThe operation cannot be undone.")
                         .arg(files.constFirst())
                         .arg(totalSize)
                   : tr("Delete %1 files from PASE (%2 bytes total)?\n\n%3\n\nThe operation cannot be undone.")
                         .arg(files.size())
                         .arg(totalSize)
                         .arg(files.join(QLatin1Char('\n'))))
            : tr("Delete %1 file(s)?").arg(files.size());
    const auto reply = QMessageBox::question(
        this, tr("Delete"), confirmation,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        if (deviceMgr_->isPrinterClassDevicePresent()) {
            activeOperationId_ =
                deviceMgr_->queueDeleteMediaOperation(QString(), files);
            syncOperationPanel(deviceMgr_->operationSnapshot());
        } else {
            deviceMgr_->deleteMedia(files);
        }
    }
}

void PanoramaPage::onRefreshClicked() {
    if (!refreshBtn_->isEnabled()) {
        return;
    }
    refreshPending_ = true;
    updateActionAvailability();
    deviceMgr_->refreshMediaList();
}

void PanoramaPage::onBrightnessChanged(int value) {
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        pendingBrightness_ = qBound(0, value, 100);
        {
            const QSignalBlocker blocker(brightnessSlider_);
            brightnessSlider_->setValue(pendingBrightness_);
        }
        brightnessLabel_->setText(
            QString::number(pendingBrightness_));
        schedulePendingBrightness();
        return;
    }
    if (!deviceMgr_->isConnected()) {
        return;
    }
    deviceMgr_->setBrightness(value);
}

void PanoramaPage::schedulePendingBrightness() {
    if (brightnessDispatchQueued_ || pendingBrightness_ < 0) {
        return;
    }
    brightnessDispatchQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            brightnessDispatchQueued_ = false;
            submitPendingBrightness();
        },
        Qt::QueuedConnection);
}

void PanoramaPage::submitPendingBrightness() {
    if (pendingBrightness_ < 0 ||
        !brightnessOperationId_.isEmpty() ||
        !displayMutationReady_) {
        return;
    }
    if (!deviceMgr_->isPrinterClassDevicePresent() ||
        !deviceMgr_->isPrinterDisplaySessionActive() ||
        !deviceMgr_->displayState().valid) {
        pendingBrightness_ = -1;
        onDisplayStateUpdated(deviceMgr_->displayState());
        emit statusMessage(tr(
            "The PASE display state is not ready yet. Reconnect the device and wait for synchronization."));
        return;
    }
    if (!deviceMgr_->operationSnapshot()
             .activeOperationId.isEmpty()) {
        return;
    }

    const int target = pendingBrightness_;
    pendingBrightness_ = -1;
    if (deviceMgr_->displayState().brightness == target) {
        onDisplayStateUpdated(deviceMgr_->displayState());
        return;
    }

    const QString operationId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    brightnessOperationId_ = operationId;
    brightnessOperationTarget_ = target;
    brightnessBaseRevision_ =
        deviceMgr_->displayState().revision;
    brightnessOperationSucceeded_ = false;
    brightnessReadbackConfirmed_ = false;
    displayMutationReady_ = false;

    TryxRuntimeDisplayMutation mutation;
    mutation.brightnessPresent = true;
    mutation.brightness = target;
    TryxRuntimeApplyRequest request;
    request.display = mutation;
    activeOperationId_ =
        deviceMgr_->queueApplyOperation(operationId, request);
    const TryxRuntimeOperationInfo operation =
        deviceMgr_->operationInfo(activeOperationId_);
    if (operation.state == QStringLiteral("Failed") ||
        operation.state == QStringLiteral("Cancelled")) {
        displayMutationReady_ = false;
        finishBrightnessPipeline(false);
        emit statusMessage(operation.message);
    }
    syncOperationPanel(deviceMgr_->operationSnapshot());
}

void PanoramaPage::updateBrightnessPipelineFromState(
    const TryxRuntimeDisplayState &state) {
    if (brightnessOperationId_.isEmpty() ||
        !state.valid ||
        state.revision <= brightnessBaseRevision_ ||
        state.brightness != brightnessOperationTarget_) {
        return;
    }
    brightnessReadbackConfirmed_ = true;
    if (brightnessOperationSucceeded_) {
        finishBrightnessPipeline(true);
    }
}

void PanoramaPage::finishBrightnessPipeline(
    bool keepPending) {
    brightnessOperationId_.clear();
    brightnessOperationTarget_ = -1;
    brightnessBaseRevision_ = 0;
    brightnessOperationSucceeded_ = false;
    brightnessReadbackConfirmed_ = false;
    if (!keepPending) {
        pendingBrightness_ = -1;
    }
    if (pendingBrightness_ < 0 &&
        deviceMgr_->displayState().valid) {
        const QSignalBlocker blocker(brightnessSlider_);
        brightnessSlider_->setValue(
            deviceMgr_->displayState().brightness);
        brightnessLabel_->setText(QString::number(
            deviceMgr_->displayState().brightness));
    }
    updateActionAvailability();
    if (keepPending && displayMutationReady_) {
        schedulePendingBrightness();
    }
}

void PanoramaPage::submitDisplayMutation(
    const TryxRuntimeDisplayMutation &mutation) {
    if (!deviceMgr_->isPrinterClassDevicePresent()) {
        return;
    }
    if (!deviceMgr_->isPrinterDisplaySessionActive() ||
        !deviceMgr_->displayState().valid) {
        onDisplayStateUpdated(deviceMgr_->displayState());
        emit statusMessage(tr(
            "The PASE display state is not ready yet. Reconnect the device and wait for synchronization."));
        return;
    }

    TryxRuntimeApplyRequest request;
    request.display = mutation;
    displayMutationReady_ = false;
    activeOperationId_ =
        deviceMgr_->queueApplyOperation(QString(), request);
    const TryxRuntimeOperationInfo operation =
        deviceMgr_->operationInfo(activeOperationId_);
    if (operation.state == QStringLiteral("Failed") ||
        operation.state == QStringLiteral("Cancelled")) {
        displayMutationReady_ = false;
        onDisplayStateUpdated(deviceMgr_->displayState());
        emit statusMessage(operation.message);
    }
    syncOperationPanel(deviceMgr_->operationSnapshot());
}

void PanoramaPage::selectDisplayMedia(const QStringList &media) {
    if (!fileList_) {
        return;
    }
    const QSignalBlocker blocker(fileList_);
    fileList_->clearSelection();
    for (int index = 0; index < fileList_->count(); ++index) {
        QListWidgetItem *item = fileList_->item(index);
        QString fileName = item->data(Qt::UserRole).toString();
        if (fileName.isEmpty()) {
            fileName =
                item->text().section(QLatin1Char('\n'), 0, 0);
        }
        item->setSelected(media.contains(fileName));
    }
}

void PanoramaPage::onDisplayStateUpdated(
    const TryxRuntimeDisplayState &state) {
    if (!state.valid) {
        updateActionAvailability();
        return;
    }

    updateBrightnessPipelineFromState(state);
    const int visibleBrightness =
        pendingBrightness_ >= 0
        ? pendingBrightness_
        : brightnessOperationTarget_ >= 0
            ? brightnessOperationTarget_
            : state.brightness;
    {
        const QSignalBlocker blocker(brightnessSlider_);
        brightnessSlider_->setValue(visibleBrightness);
    }
    brightnessLabel_->setText(
        QString::number(visibleBrightness));
    {
        const QSignalBlocker blocker(cbDisplayOff_);
        cbDisplayOff_->setChecked(!state.backlightEnabled);
    }
    {
        const QSignalBlocker blocker(cbMirrorMode_);
        cbMirrorMode_->setChecked(state.mirrorMode);
    }
    {
        const QSignalBlocker blocker(cbWaterfallMode_);
        cbWaterfallMode_->setChecked(state.waterfallMode);
    }

    const bool split =
        state.screenMode == QStringLiteral("Screen Splitting");
    {
        const QSignalBlocker fullBlocker(fullScreenRadio_);
        const QSignalBlocker splitBlocker(splitScreenRadio_);
        fullScreenRadio_->setChecked(!split);
        splitScreenRadio_->setChecked(split);
    }
    onScreenModeChanged();

    const QStringList leftMetrics = state.sysinfoLabels;
    QStringList allMetrics = leftMetrics;
    for (const QString &metric : state.sysinfoLabels2) {
        if (!allMetrics.contains(metric)) {
            allMetrics.append(metric);
        }
    }
    activePrinterMetrics_ = allMetrics;
    for (auto &option : metricOptions_) {
        const QSignalBlocker blocker(option.checkbox);
        option.checkbox->setChecked(
            leftMetrics.contains(option.label));
    }
    for (QCheckBox *checkbox : customMetricCheckboxes_) {
        const QString label =
            checkbox->property("protocolLabel").toString();
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(leftMetrics.contains(label));
    }
    onMetricToggled();
    updateCustomMetricsButton();

    {
        const QSignalBlocker blocker(cbCpuBadge_);
        cbCpuBadge_->setChecked(
            state.settingsBadges.contains(
                QStringLiteral("CPU Badge")));
    }
    {
        const QSignalBlocker blocker(cbGpuBadge_);
        cbGpuBadge_->setChecked(
            state.settingsBadges.contains(
                QStringLiteral("GPU Badge")));
    }
    const int positionIndex =
        positionCombo_->findData(state.settingsPosition);
    if (positionIndex >= 0) {
        const QSignalBlocker blocker(positionCombo_);
        positionCombo_->setCurrentIndex(positionIndex);
    }
    const int alignmentIndex =
        alignCombo_->findData(state.settingsAlign);
    if (alignmentIndex >= 0) {
        const QSignalBlocker blocker(alignCombo_);
        alignCombo_->setCurrentIndex(alignmentIndex);
    }
    if (QColor(state.settingsColor).isValid()) {
        textColor_ = QColor(state.settingsColor);
        textColorBtn_->setStyleSheet(
            QString("background-color: %1; color: %2; padding: 4px 12px;")
                .arg(textColor_.name())
                .arg(textColor_.lightness() > 128 ? "#000" : "#fff"));
    }

    const int playModeIndex =
        playModeCombo_->findData(state.playMode);
    if (playModeIndex >= 0) {
        const QSignalBlocker blocker(playModeCombo_);
        playModeCombo_->setCurrentIndex(playModeIndex);
    }
    if (split) {
        splitConfigWidget_->setConfiguration(
            state.media.value(0), state.media.value(1),
            state.sysinfoLabels, state.sysinfoLabels2,
            state.settingsBadges, state.settingsBadges2,
            state.playMode);
        splitConfigWidget_->setAreaSettings(
            state.settingsPosition, state.settingsColor,
            state.settingsAlign, state.settingsPosition2,
            state.settingsColor2, state.settingsAlign2);
        const auto thumbnailFor = [this](const QString &mediaFile) {
            for (int index = 0; index < fileList_->count(); ++index) {
                QListWidgetItem *item = fileList_->item(index);
                if (item->data(Qt::UserRole).toString() == mediaFile) {
                    return item->icon().pixmap(QSize(200, 150));
                }
            }
            return QPixmap();
        };
        splitConfigWidget_->assignToLeft(
            state.media.value(0),
            thumbnailFor(state.media.value(0)));
        splitConfigWidget_->assignToRight(
            state.media.value(1),
            thumbnailFor(state.media.value(1)));
    }
    selectDisplayMedia(state.media);

    if (!state.media.isEmpty()) {
        QString previewName = PRINTER_DEFAULT_PREVIEW_MAP.value(
            QFileInfo(state.media.constFirst()).fileName());
        if (!previewName.isEmpty()) {
            previewName = QFileInfo(previewName).completeBaseName();
            for (MediaTile *tile : presetTiles_) {
                const bool selected =
                    QFileInfo(tile->filePath()).completeBaseName() ==
                    previewName;
                tile->setSelected(selected);
                if (selected) {
                    selectedPresetTile_ = tile;
                }
            }
        }
    }
    updateActionAvailability();
}

void PanoramaPage::onMediaListUpdated(const QStringList &files) {
    refreshPending_ = false;
    if (deviceMgr_->hasTypedMediaCatalog()) {
        updateActionAvailability();
        return;
    }
    const QStringList selectedNames = selectedDeviceMediaNames();
    fileList_->clear();
    QDir().mkpath(THUMB_CACHE_DIR);

    for (const auto &f : files) {
        QString ext = QFileInfo(f).suffix().toUpper();
        if (ext.isEmpty()) ext = "FILE";
        QString displayText = f + "\n" + ext;

        auto *item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, f);  // Store original filename
        item->setTextAlignment(Qt::AlignCenter);

        // Try to load cached thumbnail
        QString thumbPath = thumbnailCachePathForDeviceFile(f);
        if (QFileInfo::exists(thumbPath)) {
            QPixmap pix(thumbPath);
            if (!pix.isNull()) {
                item->setIcon(QIcon(pix.scaled(120, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        } else {
            // Dark placeholder icon
            QPixmap placeholder(120, 80);
            placeholder.fill(QColor("#2a2a3a"));
            item->setIcon(QIcon(placeholder));
        }

        fileList_->addItem(item);
        item->setSelected(selectedNames.contains(f));

        if (!QFileInfo::exists(thumbPath)) {
            QString sourcePath = builtinPreviewSourceForDeviceFile(f);
            if (sourcePath.isEmpty()) {
                sourcePath = localPreviewSourceForDeviceFile(f);
            }
            cacheThumbnailForDeviceFile(f, sourcePath);
        }
    }
    if (deviceMgr_->displayState().valid) {
        onDisplayStateUpdated(deviceMgr_->displayState());
    } else {
        updateActionAvailability();
    }
    emit statusMessage(tr("Files on device: %1").arg(files.size()));
}

void PanoramaPage::onMediaCatalogUpdated(
    const TryxRuntimeMediaCatalogSnapshot &snapshot) {
    refreshPending_ = false;
    const QStringList selectedNames = selectedDeviceMediaNames();
    fileList_->clear();
    QDir().mkpath(THUMB_CACHE_DIR);

    for (const TryxRuntimeMediaEntry &entry : snapshot.entries) {
        const QString sourceText = entry.source == 2U
            ? tr("PRESET")
            : tr("UPLOAD");
        const QString sizeText = entry.size >= 1024U * 1024U
            ? tr("%1 MB").arg(
                  QString::number(static_cast<double>(entry.size) /
                                      (1024.0 * 1024.0),
                                  'f', 1))
            : tr("%1 KB").arg(
                  QString::number(static_cast<double>(entry.size) / 1024.0,
                                  'f', 1));
        auto *item = new QListWidgetItem(
            entry.name + QLatin1Char('\n') + sourceText +
            QStringLiteral("  ") + sizeText);
        item->setData(Qt::UserRole, entry.name);
        item->setData(MEDIA_SIZE_ROLE,
                      QVariant::fromValue<qulonglong>(entry.size));
        item->setData(MEDIA_SOURCE_ROLE, entry.source);
        item->setData(MEDIA_READ_ONLY_ROLE, entry.readOnly);
        item->setData(MEDIA_THUMBNAIL_KEY_ROLE, entry.thumbnailKey);
        item->setData(MEDIA_MANAGED_ORIGIN_ROLE, entry.managedOrigin);
        item->setData(MEDIA_DELETE_ALLOWED_ROLE, entry.deleteAllowed);
        item->setData(MEDIA_DELETE_BLOCK_REASON_ROLE,
                      entry.deleteBlockReason);
        item->setTextAlignment(Qt::AlignCenter);

        QString thumbnailPath;
        if (!entry.thumbnailKey.isEmpty()) {
            thumbnailPath = deviceMgr_->mediaThumbnailPath(
                entry.thumbnailKey);
        }
        const QString builtinPreview = entry.source == 2U
            ? builtinPreviewSourceForDeviceFile(entry.name)
            : QString();
        if (thumbnailPath.isEmpty() && !builtinPreview.isEmpty()) {
            thumbnailPath = thumbnailCachePathForDeviceFile(entry.name);
        }
        QPixmap pix(thumbnailPath);
        if (!pix.isNull()) {
            item->setIcon(QIcon(pix.scaled(
                120, 80, Qt::KeepAspectRatio,
                Qt::SmoothTransformation)));
        } else {
            QPixmap placeholder(120, 80);
            placeholder.fill(QColor("#2a2a3a"));
            item->setIcon(QIcon(placeholder));
        }
        fileList_->addItem(item);
        item->setSelected(selectedNames.contains(entry.name));

        if (entry.thumbnailKey.isEmpty() && !builtinPreview.isEmpty() &&
            !QFileInfo::exists(thumbnailPath)) {
            cacheThumbnailForDeviceFile(entry.name, builtinPreview);
        }
    }
    if (deviceMgr_->displayState().valid) {
        onDisplayStateUpdated(deviceMgr_->displayState());
    } else {
        updateActionAvailability();
    }
    emit statusMessage(
        tr("Files on device: %1").arg(snapshot.entries.size()));
}

void PanoramaPage::onMediaUploaded(const QString &filename) {
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        Q_UNUSED(filename);
        return;
    }
    setUploadBusy(false);
    if (!pendingUploadSourcePath_.isEmpty()) {
        cacheThumbnailForDeviceFile(filename, pendingUploadSourcePath_);
        pendingUploadSourcePath_.clear();
    }
    emit statusMessage(tr("Uploaded: %1").arg(filename));
    deviceMgr_->refreshMediaList();
}

void PanoramaPage::onMediaDeleted() {
    emit statusMessage(tr("Files deleted"));
    deviceMgr_->refreshMediaList();
}

void PanoramaPage::onUploadStatus(const QString &status) {
    emit statusMessage(status);
}

void PanoramaPage::onOperationChanged(
    const TryxRuntimeOperationInfo &info, quint64 revision) {
    Q_UNUSED(revision);
    const bool terminal = info.state == QStringLiteral("Succeeded") ||
                          info.state == QStringLiteral("Failed") ||
                          info.state == QStringLiteral("Cancelled") ||
                          info.state == QStringLiteral("RetryAvailable");
    const bool wasActive = info.id == activeOperationId_;
    if (terminal && info.state != QStringLiteral("RetryAvailable")) {
        uploadSourcePaths_.remove(info.id);
    }
    if (terminal && wasActive) {
        pendingUploadSourcePath_.clear();
    }
    if (!info.message.isEmpty()) {
        emit statusMessage(info.message);
    }
    if (terminal && info.id == printerMetricsOperationId_) {
        printerMetricsOperationId_.clear();
        pendingPrinterMetrics_.clear();
    }
    syncOperationPanel(deviceMgr_->operationSnapshot());
    if (terminal && info.id == brightnessOperationId_) {
        if (info.state == QStringLiteral("Succeeded")) {
            brightnessOperationSucceeded_ = true;
            updateBrightnessPipelineFromState(
                deviceMgr_->displayState());
        } else {
            displayMutationReady_ = false;
            finishBrightnessPipeline(false);
        }
    }
}

void PanoramaPage::onMetricsStateUpdated(
    const TryxRuntimeMetricsState &state) {
    if (state.deviceSerial.trimmed().isEmpty()) {
        activePrinterMetrics_.clear();
        availablePrinterMetrics_.clear();
        for (auto &option : metricOptions_) {
            const QSignalBlocker blocker(option.checkbox);
            option.checkbox->setChecked(false);
        }
        for (QCheckBox *checkbox : customMetricCheckboxes_) {
            const QSignalBlocker blocker(checkbox);
            checkbox->setChecked(false);
            checkbox->setEnabled(true);
        }
        splitConfigWidget_->setAvailableMetrics({});
        onMetricToggled();
        updateCustomMetricsButton();
        metricsTimer_->stop();
        if (metricsRunning_) {
            metricsRunning_ = false;
            emit metricsRunningChanged(false);
        }
        if (state.diagnostic.isEmpty()) {
            metricsStatusLabel_->clear();
            metricsStatusLabel_->setStyleSheet("color: #888;");
        } else {
            metricsStatusLabel_->setText(state.diagnostic);
            metricsStatusLabel_->setStyleSheet("color: #ff7675;");
        }
        return;
    }
    activePrinterMetrics_ = state.metrics;
    availablePrinterMetrics_ = state.availableMetrics;
    splitConfigWidget_->setAvailableMetrics(state.availableMetrics);

    for (auto &option : metricOptions_) {
        const QSignalBlocker blocker(option.checkbox);
        option.checkbox->setChecked(state.metrics.contains(option.label));
    }
    for (QCheckBox *checkbox : customMetricCheckboxes_) {
        const QString label =
            checkbox->property("protocolLabel").toString();
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(state.metrics.contains(label));
        const bool sensorAvailable = state.availableMetrics.isEmpty() ||
                                     state.availableMetrics.contains(label);
        checkbox->setEnabled(checkbox->isChecked() || sensorAvailable);
    }
    updateCustomMetricsButton();
    const int alignmentIndex = alignCombo_->findData(state.alignment);
    if (alignmentIndex >= 0) {
        alignCombo_->setCurrentIndex(alignmentIndex);
    }
    textColor_ = QColor::fromRgb(state.textColor & 0x00FFFFFFU);
    textColorBtn_->setStyleSheet(
        QString("background-color: %1; color: %2; padding: 4px 12px;")
            .arg(textColor_.name())
            .arg(textColor_.lightness() > 128 ? "#000" : "#fff"));
    onMetricToggled();
    savePageState();

    metricsTimer_->stop();
    const bool running = state.enabled && state.samplingActive &&
                         !state.metrics.isEmpty();
    if (metricsRunning_ != running) {
        metricsRunning_ = running;
        emit metricsRunningChanged(running);
    }
    if (!state.diagnostic.isEmpty()) {
        metricsStatusLabel_->setText(state.diagnostic);
        metricsStatusLabel_->setStyleSheet("color: #ff7675;");
        return;
    }
    if (running) {
        metricsStatusLabel_->setText(
            tr("Metrics active in background runtime"));
        metricsStatusLabel_->setStyleSheet("color: #00b894;");
    } else if (state.enabled) {
        metricsStatusLabel_->setText(
            tr("Metrics configured; waiting for the PASE session"));
        metricsStatusLabel_->setStyleSheet("color: #fdcb6e;");
    } else {
        metricsStatusLabel_->clear();
        metricsStatusLabel_->setStyleSheet("color: #888;");
    }
}

void PanoramaPage::onRetryClicked() {
    if (retryOperationId_.isEmpty()) {
        return;
    }
    if (deviceMgr_->isPrinterClassDevicePresent() &&
        !deviceMgr_->isPrinterDisplaySessionActive()) {
        emit statusMessage(tr(
            "Retry is blocked until PASE is power-cycled and its display session becomes active."));
        return;
    }
    const QString sourcePath = uploadSourcePaths_.value(retryOperationId_);
    const QString newOperationId =
        deviceMgr_->retryOperation(retryOperationId_, QString());
    const TryxRuntimeOperationInfo operation =
        deviceMgr_->operationInfo(newOperationId);
    if (!sourcePath.isEmpty() &&
        operation.state != QStringLiteral("Failed") &&
        operation.state != QStringLiteral("Cancelled")) {
        uploadSourcePaths_.insert(newOperationId, sourcePath);
    }
    syncOperationPanel(deviceMgr_->operationSnapshot());
}

void PanoramaPage::onCancelClicked() {
    if (!activeOperationId_.isEmpty()) {
        deviceMgr_->cancelOperation(activeOperationId_);
    }
}

void PanoramaPage::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        if (dropZone_) {
            dropZone_->setStyleSheet(
                "QLabel {"
                "  border: 2px dashed #6c5ce7;"
                "  border-radius: 8px;"
                "  padding: 20px;"
                "  color: #6c5ce7;"
                "  font-size: 13px;"
                "  background: rgba(108, 92, 231, 30);"
                "}");
        }
    }
}

void PanoramaPage::dropEvent(QDropEvent *event) {
    if (dropZone_) {
        dropZone_->setStyleSheet(
            "QLabel {"
            "  border: 2px dashed #555;"
            "  border-radius: 8px;"
            "  padding: 20px;"
            "  color: #888;"
            "  font-size: 13px;"
            "}");
    }

    if (deviceMgr_->isPrinterClassDevicePresent() &&
        !deviceMgr_->isPrinterDisplaySessionActive()) {
        emit statusMessage(tr(
            "The PASE display session is not ready. Reconnect or power-cycle the device and wait for it to become active."));
        return;
    }

    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            pendingUploadSourcePath_ = url.toLocalFile();
            if (deviceMgr_->isPrinterClassDevicePresent()) {
                activeOperationId_ = deviceMgr_->queueUploadOperation(
                    QString(), pendingUploadSourcePath_, false);
                const TryxRuntimeOperationInfo operation =
                    deviceMgr_->operationInfo(activeOperationId_);
                if (operation.state != QStringLiteral("Failed") &&
                    operation.state != QStringLiteral("Cancelled")) {
                    uploadSourcePaths_.insert(activeOperationId_,
                                              pendingUploadSourcePath_);
                }
                syncOperationPanel(deviceMgr_->operationSnapshot());
            } else {
                setUploadBusy(true);
                deviceMgr_->uploadMedia(pendingUploadSourcePath_);
            }
            break;
        }
    }
}

int PanoramaPage::calculateGridColumns() const {
    int availableWidth = presetGridWidget_ ? presetGridWidget_->width() : 810;
    int cols = availableWidth / 270;
    return qMax(2, cols);
}

void PanoramaPage::rebuildPresetGrid() {
    // Remove all widgets from grid without deleting them
    while (presetGrid_->count() > 0) {
        presetGrid_->takeAt(0);
    }

    int columns = calculateGridColumns();
    int row = 0, col = 0;
    for (auto *tile : presetTiles_) {
        presetGrid_->addWidget(tile, row, col);
        col++;
        if (col >= columns) {
            col = 0;
            row++;
        }
    }
}

void PanoramaPage::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!presetTiles_.isEmpty()) {
        rebuildPresetGrid();
    }
}
