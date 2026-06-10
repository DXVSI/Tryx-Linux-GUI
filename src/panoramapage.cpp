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
#include <memory>

static const int TILE_WIDTH = 250;
static const int TILE_IMG_HEIGHT = 140;
static const QString THUMB_CACHE_DIR = "/tmp/tryx-panorama/thumbnails";

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

PanoramaPage::PanoramaPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr) {

    monitor_ = new SystemMonitor(this);
    metricsTimer_ = new QTimer(this);

    setupUi();
    restorePageState();

    connect(metricsTimer_, &QTimer::timeout, this, &PanoramaPage::onSendMetrics);

    // Device signals
    connect(deviceMgr_, &DeviceManager::mediaListUpdated, this, &PanoramaPage::onMediaListUpdated);
    connect(deviceMgr_, &DeviceManager::mediaUploaded, this, &PanoramaPage::onMediaUploaded);
    connect(deviceMgr_, &DeviceManager::mediaDeleted, this, &PanoramaPage::onMediaDeleted);
    connect(deviceMgr_, &DeviceManager::uploadStatus, this, &PanoramaPage::onUploadStatus);
    connect(deviceMgr_, &DeviceManager::brightnessChanged, this,
            [this](int val) { brightnessLabel_->setText(QString::number(val)); });
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

    auto *previewAudio = new QAudioOutput(this);
    previewAudio->setVolume(0);
    previewPlayer_ = new QMediaPlayer(this);
    previewPlayer_->setAudioOutput(previewAudio);
    previewSink_ = new QVideoSink(this);
    previewPlayer_->setVideoOutput(previewSink_);
    connect(previewSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        QVideoFrame f = frame;
        if (f.map(QVideoFrame::ReadOnly)) {
            QImage img = f.toImage();
            f.unmap();
            if (!img.isNull()) {
                previewLabel_->setPixmap(QPixmap::fromImage(
                    img.scaled(previewLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            }
        }
    });

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
        {QT_TR_NOOP("CPU Temperature"),        "CPU Temperature",        "C"},
        {QT_TR_NOOP("CPU Frequency"),          "CPU Frequency",          "MHz"},
        {QT_TR_NOOP("CPU Usage"),              "CPU Usage",              "%"},
        {QT_TR_NOOP("CPU Voltage"),            "CPU Voltage",            "V"},
        {QT_TR_NOOP("GPU Temperature"),        "GPU Temperature",        "C"},
        {QT_TR_NOOP("GPU Frequency"),          "GPU Frequency",          "MHz"},
        {QT_TR_NOOP("GPU Usage"),              "GPU Usage",              "%"},
        {QT_TR_NOOP("GPU Voltage"),            "GPU Voltage",            "V"},
        {QT_TR_NOOP("Hard Disk Temperature"),  "Hard Disk Temperature",  "C"},
        {QT_TR_NOOP("Motherboard Temperature"), "Motherboard Temperature", "C"},
        {QT_TR_NOOP("Memory Frequency"),       "Memory Frequency",       "MHz"},
        {QT_TR_NOOP("Memory Utilization"),     "Memory Utilization",     "%"},
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

    controlsLayout->addWidget(new QLabel(tr("Position:")));
    positionCombo_ = new QComboBox;
    positionCombo_->addItem(tr("Top"), "Top");
    positionCombo_->addItem(tr("Center"), "Center");
    positionCombo_->addItem(tr("Bottom"), "Bottom");
    controlsLayout->addWidget(positionCombo_);

    controlsLayout->addWidget(new QLabel(tr("Align:")));
    alignCombo_ = new QComboBox;
    alignCombo_->addItem(tr("Left"), "Left");
    alignCombo_->addItem(tr("Center"), "Center");
    alignCombo_->addItem(tr("Right"), "Right");
    controlsLayout->addWidget(alignCombo_);

    textColorBtn_ = new QPushButton(tr("Color"));
    textColorBtn_->setStyleSheet("background-color: #FFFFFF; color: #000; padding: 4px 12px;");
    textColorBtn_->setMaximumWidth(80);
    connect(textColorBtn_, &QPushButton::clicked, this, &PanoramaPage::onChooseTextColor);
    controlsLayout->addWidget(textColorBtn_);

    cbCpuBadge_ = new QCheckBox(tr("CPU Badge"));
    cbCpuBadge_->setStyleSheet("color: #ccc;");
    cbGpuBadge_ = new QCheckBox(tr("GPU Badge"));
    cbGpuBadge_->setStyleSheet("color: #ccc;");
    controlsLayout->addWidget(cbCpuBadge_);
    controlsLayout->addWidget(cbGpuBadge_);

    controlsLayout->addStretch();
    layout->addLayout(controlsLayout);

    // Save button
    auto *sendLayout = new QHBoxLayout;
    sendLayout->addStretch();

    auto *saveBtn = new QPushButton(tr("Save"));
    saveBtn->setMinimumHeight(36);
    saveBtn->setMinimumWidth(120);
    saveBtn->setStyleSheet(
        "QPushButton { background: #00b894; color: white; border: none; border-radius: 4px; padding: 8px 24px; font-weight: bold; font-size: 13px; }"
        "QPushButton:hover { background: #00a381; }");
    connect(saveBtn, &QPushButton::clicked, this, &PanoramaPage::onPresetSave);
    sendLayout->addWidget(saveBtn);

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
    splitScreenRadio_ = new QRadioButton(tr("Screen Splitting"));
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

    screenModeCombo_ = new QComboBox;
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
        QT_TR_NOOP("CPU Usage"), QT_TR_NOOP("CPU Voltage"),
        QT_TR_NOOP("GPU Temperature"), QT_TR_NOOP("GPU Frequency"),
        QT_TR_NOOP("GPU Usage"), QT_TR_NOOP("GPU Voltage"),
        QT_TR_NOOP("Hard Disk Temperature"), QT_TR_NOOP("Motherboard Temperature"),
        QT_TR_NOOP("Memory Frequency"), QT_TR_NOOP("Memory Utilization"),
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
            customMetricsBtn_->setText(QString::fromUtf8("%1 / 3 \u25BC").arg(count));
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
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 0);
    progressBar_->setVisible(false);
    progressBar_->setMaximumHeight(20);
    uploadLayout->addWidget(progressBar_);
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
    setDisplayBtn_ = new QPushButton;
    setDisplayBtn_->hide();
    deleteBtn_ = new QPushButton;
    deleteBtn_->hide();
    refreshBtn_ = new QPushButton;
    refreshBtn_->hide();

    // Refresh + Save row
    auto *actionLayout = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(tr("Refresh"));
    refreshBtn->setStyleSheet(
        "QPushButton { background: #3d3d4d; color: #ddd; border: none; border-radius: 4px; padding: 6px 12px; }"
        "QPushButton:hover { background: #4d4d5d; }");
    connect(refreshBtn, &QPushButton::clicked, this, &PanoramaPage::onRefreshClicked);
    actionLayout->addWidget(refreshBtn);

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
    brightnessSlider_->setRange(0, 100);
    brightnessSlider_->setValue(75);
    brightnessSlider_->setMaximumWidth(200);
    settingsLayout->addWidget(brightnessSlider_);
    brightnessLabel_ = new QLabel("75");
    brightnessLabel_->setMinimumWidth(30);
    settingsLayout->addWidget(brightnessLabel_);

    connect(brightnessSlider_, &QSlider::valueChanged, this,
            [this](int val) { brightnessLabel_->setText(QString::number(val)); });
    connect(brightnessSlider_, &QSlider::sliderReleased, this,
            [this]() { onBrightnessChanged(brightnessSlider_->value()); });

    // Sleep mode
    cbSleepMode_ = new QCheckBox(tr("Sleep Mode"));
    cbSleepMode_->setStyleSheet("color: #ccc;");
    settingsLayout->addWidget(cbSleepMode_);

    // Mirror mode
    cbMirrorMode_ = new QCheckBox(tr("Mirror Mode"));
    cbMirrorMode_->setStyleSheet("color: #ccc;");
    settingsLayout->addWidget(cbMirrorMode_);

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
    previewPlayer_->stop();
    previewLabel_->hide();

    if (selectedPresetTile_) {
        QString path = selectedPresetTile_->filePath();
        QString ext = path.section('.', -1).toLower();
        if (ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "avi" || ext == "mov") {
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

void PanoramaPage::onMetricToggled() {
    int count = 0;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) count++;
    }

    selectionCountLabel_->setText(tr("Selected: %1 / 3").arg(count));

    for (auto &opt : metricOptions_) {
        if (!opt.checkbox->isChecked()) {
            opt.checkbox->setEnabled(count < 3);
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
        textColor_ = QColor(settings.value("display/textColor").toString());
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
}

void PanoramaPage::onPresetSave() {
    applyScreenConfig();

    // Auto-start metrics sending after Save (like KANALI)
    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }

    if (!labels.isEmpty()) {
        // Delay metrics start to let device process screen config first
        QTimer::singleShot(2000, this, [this]() { startMetrics(); });
    }

    // Persist current page state
    savePageState();

    emit statusMessage(tr("Configuration applied"));
}

void PanoramaPage::startMetrics() {
    if (metricsRunning_) return;

    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }
    if (labels.isEmpty()) return;

    metricsRunning_ = true;
    metricsTimer_->start(2000);
    emit metricsRunningChanged(true);
    metricsStatusLabel_->setText(tr("Metrics active"));
    metricsStatusLabel_->setStyleSheet("color: #00b894;");

    onSendMetrics();
}

void PanoramaPage::stopMetrics() {
    if (!metricsRunning_) return;

    metricsRunning_ = false;
    metricsTimer_->stop();
    emit metricsRunningChanged(false);
    metricsStatusLabel_->setText("");
    metricsStatusLabel_->setStyleSheet("color: #888;");
}

void PanoramaPage::onSendMetrics() {
    monitor_->update();
    auto metrics = monitor_->currentMetrics();

    QStringList labels, values, units;

    // Always send all available metrics - device expects complete PcInfo
    labels << "CPU Temperature";
    values << QString::number(metrics.cpu.temperature, 'f', 1);
    units << "C";

    labels << "CPU Usage";
    values << QString::number(metrics.cpu.usagePercent, 'f', 1);
    units << "%";

    labels << "CPU Frequency";
    values << QString::number(metrics.cpu.frequencyMHz, 'f', 0);
    units << "MHz";

    if (!metrics.gpus.isEmpty()) {
        labels << "GPU Temperature";
        values << QString::number(metrics.gpus[0].temperature, 'f', 1);
        units << "C";

        labels << "GPU Usage";
        values << QString::number(metrics.gpus[0].usagePercent, 'f', 1);
        units << "%";

        labels << "GPU Frequency";
        values << QString::number(metrics.gpus[0].frequencyMHz, 'f', 0);
        units << "MHz";

        labels << "GPU Voltage";
        values << QString::number(metrics.gpus[0].voltageMV / 1000.0, 'f', 3);
        units << "V";
    } else {
        labels << "GPU Temperature";
        values << "0";
        units << "C";

        labels << "GPU Usage";
        values << "0";
        units << "%";

        labels << "GPU Frequency";
        values << "0";
        units << "MHz";

        labels << "GPU Voltage";
        values << "0";
        units << "V";
    }

    labels << "Memory Utilization";
    values << QString::number(metrics.ram.usagePercent, 'f', 1);
    units << "%";

    labels << "Hard Disk Temperature";
    values << QString::number(metrics.disk.temperature, 'f', 1);
    units << "C";

    labels << "Motherboard Temperature";
    values << "0";
    units << "C";

    // Print debug for selected metrics only
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            int idx = labels.indexOf(opt.label);
            if (idx >= 0) {
                fprintf(stderr, "[sysinfo] %s = %s %s\n",
                        labels[idx].toStdString().c_str(),
                        values[idx].toStdString().c_str(),
                        units[idx].toStdString().c_str());
            }
        }
    }

    deviceMgr_->sendSysinfo(labels, values, units);
    metricsStatusLabel_->setText(tr("Metrics active"));
}

// Customization tab slots

void PanoramaPage::onUploadClicked() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select media file"), QString(),
        "Media (*.mp4 *.webm *.mkv *.avi *.mov *.gif *.jpg *.jpeg *.png *.bmp *.webp)");

    if (!path.isEmpty()) {
        progressBar_->setVisible(true);
        uploadBtn_->setEnabled(false);
        deviceMgr_->uploadMedia(path);
    }
}

void PanoramaPage::onUploadBuiltinClicked() {
    // Upload selected preset tile to device
    if (!selectedPresetTile_) {
        emit statusMessage(tr("Select a video from the library"));
        return;
    }

    progressBar_->setVisible(true);
    deviceMgr_->uploadMedia(selectedPresetTile_->filePath());
}

void PanoramaPage::onScreenModeChanged() {
    bool isSplit = splitScreenRadio_->isChecked();
    fullScreenControls_->setVisible(!isSplit);
    splitConfigWidget_->setVisible(isSplit);
}

void PanoramaPage::onCustomSave() {
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
        QString playMode = splitConfigWidget_->playMode();

        fprintf(stderr, "[panorama] split save: left=%lld right=%lld leftMetrics=%lld rightMetrics=%lld\n",
                (long long)leftMedia.size(), (long long)rightMedia.size(),
                (long long)leftMetrics.size(), (long long)rightMetrics.size());

        bool waterfall = splitConfigWidget_->waterfallMode();
        fprintf(stderr, "[waterfallMode] user=%d\n", waterfall);

        deviceMgr_->setScreenConfig(
            allMedia,
            "2:1",
            "Screen Splitting",
            playMode,
            leftMetrics,
            "Top",
            "#FFFFFF",
            "Left",
            {},    // badges left
            0,
            QString(),  // no preset
            rightMetrics,
            {},    // badges right
            waterfall
        );

        // Start metrics sending if any metrics selected
        if (!leftMetrics.isEmpty() || !rightMetrics.isEmpty()) {
            QTimer::singleShot(2000, this, [this]() { startMetrics(); });
        }

        emit statusMessage(tr("Screen Splitting configuration applied"));
    } else {
        // Full Screen mode - use selected files from list
        auto selected = fileList_->selectedItems();
        if (selected.isEmpty()) {
            emit statusMessage(tr("Select files to display"));
            return;
        }

        QStringList media;
        for (auto *item : selected) {
            QString filename = item->data(Qt::UserRole).toString();
            if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
            media << filename;
        }

        QString ratio = ratioCombo_->currentText();
        QString playMode = playModeCombo_->currentData().toString();

        // Collect selected metrics
        QStringList metrics;
        for (auto *cb : customMetricCheckboxes_)
            if (cb->isChecked()) metrics << cb->property("protocolLabel").toString();

        deviceMgr_->setScreenConfig(media, ratio, "Full Screen", playMode, metrics,
            "Top", "#FFFFFF", "Left", {}, 0);

        if (!metrics.isEmpty()) {
            QTimer::singleShot(2000, this, [this]() { startMetrics(); });
        }

        emit statusMessage(tr("Full Screen configuration applied"));
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
            // Try to load cached thumbnail
            QString thumbName = filename;
            thumbName.replace(' ', '_');
            thumbName += ".jpg";
            QString thumbPath = THUMB_CACHE_DIR + "/" + thumbName;
            if (QFileInfo::exists(thumbPath)) {
                thumb = QPixmap(thumbPath);
            }
            splitConfigWidget_->assignToLeft(filename, thumb);
        });
        connect(setRight, &QAction::triggered, this, [this, item]() {
            QString filename = item->data(Qt::UserRole).toString();
            if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
            QPixmap thumb;
            // Try to load cached thumbnail
            QString thumbName = filename;
            thumbName.replace(' ', '_');
            thumbName += ".jpg";
            QString thumbPath = THUMB_CACHE_DIR + "/" + thumbName;
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
            deviceMgr_->setScreenConfig(media, ratio, "Full Screen", playMode);
            emit statusMessage(tr("Screen config applied"));
        });
    }

    menu.addSeparator();
    auto *deleteAction = menu.addAction(tr("Delete"));
    connect(deleteAction, &QAction::triggered, this, [this, item]() {
        QString filename = item->data(Qt::UserRole).toString();
        if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
        QStringList files;
        files << filename;
        auto reply = QMessageBox::question(this, tr("Delete"),
                                           tr("Delete %1?").arg(filename));
        if (reply == QMessageBox::Yes) {
            deviceMgr_->deleteMedia(files);
        }
    });

    menu.exec(fileList_->mapToGlobal(pos));
}

void PanoramaPage::onSetDisplayClicked() {
    auto selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to display"));
        return;
    }

    QStringList media;
    for (auto *item : selected) {
        QString filename = item->data(Qt::UserRole).toString();
        if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
        media << filename;
    }

    QString ratio = ratioCombo_ ? ratioCombo_->currentText() : "2:1";
    QString screenMode = screenModeCombo_ ? screenModeCombo_->currentData().toString() : "Full Screen";
    QString playMode = playModeCombo_ ? playModeCombo_->currentData().toString() : "Single";

    deviceMgr_->setScreenConfig(media, ratio, screenMode, playMode);
    emit statusMessage(tr("Screen config applied"));
}

void PanoramaPage::onDeleteClicked() {
    auto selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to delete"));
        return;
    }

    QStringList files;
    for (auto *item : selected) {
        QString filename = item->data(Qt::UserRole).toString();
        if (filename.isEmpty()) filename = item->text().section('\n', 0, 0);
        files << filename;
    }

    auto reply = QMessageBox::question(this, tr("Delete"),
                                       tr("Delete %1 file(s)?").arg(files.size()));
    if (reply == QMessageBox::Yes) {
        deviceMgr_->deleteMedia(files);
    }
}

void PanoramaPage::onRefreshClicked() {
    deviceMgr_->refreshMediaList();
}

void PanoramaPage::onBrightnessChanged(int value) {
    fprintf(stderr, "[brightness] %d\n", value);
    deviceMgr_->setBrightness(value);
}

void PanoramaPage::onMediaListUpdated(const QStringList &files) {
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
        QString thumbName = QString(f).replace(' ', '_') + ".jpg";
        QString thumbPath = THUMB_CACHE_DIR + "/" + thumbName;
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
    }
    emit statusMessage(tr("Files on device: %1").arg(files.size()));
}

void PanoramaPage::onMediaUploaded(const QString &filename) {
    progressBar_->setVisible(false);
    uploadBtn_->setEnabled(true);
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

    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            progressBar_->setVisible(true);
            uploadBtn_->setEnabled(false);
            deviceMgr_->uploadMedia(url.toLocalFile());
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
