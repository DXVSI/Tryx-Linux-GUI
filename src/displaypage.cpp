#include "displaypage.h"
#include "devicemanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QProcess>
#include <QPixmap>
#include <QMouseEvent>

static const int TILE_WIDTH = 200;
static const int TILE_IMG_HEIGHT = 100;
static const int GRID_COLUMNS = 3;
static const QString THUMB_CACHE_DIR = "/tmp/tryx-panorama/thumbnails";

// --- MediaTile ---

MediaTile::MediaTile(const MediaEntry &entry, QWidget *parent)
    : QFrame(parent), entry_(entry) {
    setFixedSize(TILE_WIDTH, TILE_IMG_HEIGHT + 50);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    imageLabel_ = new QLabel;
    imageLabel_->setFixedSize(TILE_WIDTH - 8, TILE_IMG_HEIGHT);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setStyleSheet("background: #1a1a1a; border-radius: 4px;");
    imageLabel_->setText("...");
    layout->addWidget(imageLabel_);

    nameLabel_ = new QLabel(entry_.fileName);
    nameLabel_->setAlignment(Qt::AlignCenter);
    nameLabel_->setWordWrap(false);
    QFont nameFont = nameLabel_->font();
    nameFont.setPointSize(8);
    nameFont.setBold(true);
    nameLabel_->setFont(nameFont);
    nameLabel_->setMaximumWidth(TILE_WIDTH - 8);
    layout->addWidget(nameLabel_);

    double sizeMB = entry_.sizeBytes / (1024.0 * 1024.0);
    infoLabel_ = new QLabel(QString("%1 MB  %2").arg(sizeMB, 0, 'f', 1).arg(entry_.format));
    infoLabel_->setAlignment(Qt::AlignCenter);
    QFont infoFont = infoLabel_->font();
    infoFont.setPointSize(7);
    infoLabel_->setFont(infoFont);
    infoLabel_->setStyleSheet("color: #888;");
    layout->addWidget(infoLabel_);

    updateStyle();
}

void MediaTile::setSelected(bool sel) {
    selected_ = sel;
    updateStyle();
}

void MediaTile::setThumbnail(const QPixmap &pix) {
    thumb_ = pix;
    if (!pix.isNull()) {
        imageLabel_->setPixmap(pix.scaled(imageLabel_->size(),
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
        imageLabel_->setText({});
    }
}

void MediaTile::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked(this);
    }
    QFrame::mousePressEvent(event);
}

void MediaTile::updateStyle() {
    if (selected_) {
        setStyleSheet(
            "MediaTile {"
            "  border: 2px solid #4CAF50;"
            "  border-radius: 6px;"
            "  background: rgba(76, 175, 80, 40);"
            "}");
    } else {
        setStyleSheet(
            "MediaTile {"
            "  border: 2px solid transparent;"
            "  border-radius: 6px;"
            "  background: #2a2a2a;"
            "}"
            "MediaTile:hover {"
            "  border: 2px solid #555;"
            "  background: #333;"
            "}");
    }
}

// --- DisplayPage ---

DisplayPage::DisplayPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr) {
    setupUi();

    connect(deviceMgr_, &DeviceManager::mediaListUpdated, this, &DisplayPage::onMediaListUpdated);
    connect(deviceMgr_, &DeviceManager::mediaUploaded, this, &DisplayPage::onMediaUploaded);
    connect(deviceMgr_, &DeviceManager::mediaDeleted, this, &DisplayPage::onMediaDeleted);
    connect(deviceMgr_, &DeviceManager::uploadStatus, this, &DisplayPage::onUploadStatus);
    connect(deviceMgr_, &DeviceManager::brightnessChanged, this,
            [this](int val) { brightnessLabel_->setText(QString::number(val)); });
}

QString DisplayPage::builtinMediaDir() {
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/../media",
        appDir + "/../../media",
        appDir + "/media",
    };
    for (const auto &path : candidates) {
        QDir dir(path);
        if (dir.exists() && !dir.isEmpty()) {
            return dir.absolutePath();
        }
    }
    return {};
}

void DisplayPage::setupUi() {
    setAcceptDrops(true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Built-in TRYX media library - thumbnail grid
    auto *builtinGroup = new QGroupBox(tr("TRYX Media Library"));
    auto *builtinOuterLayout = new QVBoxLayout(builtinGroup);

    builtinScrollArea_ = new QScrollArea;
    builtinScrollArea_->setWidgetResizable(true);
    builtinScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    builtinScrollArea_->setMinimumHeight(200);
    builtinScrollArea_->setMaximumHeight(380);
    builtinScrollArea_->setStyleSheet("QScrollArea { border: none; }");

    builtinGridWidget_ = new QWidget;
    builtinGrid_ = new QGridLayout(builtinGridWidget_);
    builtinGrid_->setSpacing(8);
    builtinGrid_->setContentsMargins(4, 4, 4, 4);
    builtinScrollArea_->setWidget(builtinGridWidget_);

    builtinOuterLayout->addWidget(builtinScrollArea_);

    uploadBuiltinBtn_ = new QPushButton(tr("Upload selected to device"));
    builtinOuterLayout->addWidget(uploadBuiltinBtn_);
    mainLayout->addWidget(builtinGroup);

    connect(uploadBuiltinBtn_, &QPushButton::clicked, this, &DisplayPage::onUploadBuiltinClicked);
    loadBuiltinMedia();

    // Drop zone
    dropZone_ = new QLabel(tr("Drag a file here\n(MP4, GIF, JPG, PNG)"));
    dropZone_->setAlignment(Qt::AlignCenter);
    dropZone_->setMinimumHeight(80);
    dropZone_->setStyleSheet(
        "QLabel {"
        "  border: 2px dashed #888;"
        "  border-radius: 8px;"
        "  padding: 20px;"
        "  color: #aaa;"
        "  font-size: 14px;"
        "}");
    mainLayout->addWidget(dropZone_);

    // Upload button
    auto *uploadLayout = new QHBoxLayout;
    uploadBtn_ = new QPushButton(tr("Upload file..."));
    uploadLayout->addWidget(uploadBtn_);
    progressBar_ = new QProgressBar;
    progressBar_->setRange(0, 0);
    progressBar_->setVisible(false);
    progressBar_->setMaximumHeight(20);
    uploadLayout->addWidget(progressBar_);
    mainLayout->addLayout(uploadLayout);

    connect(uploadBtn_, &QPushButton::clicked, this, &DisplayPage::onUploadClicked);

    // File list
    auto *fileGroup = new QGroupBox(tr("Files on device"));
    auto *fileLayout = new QVBoxLayout(fileGroup);
    fileList_ = new QListWidget;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileLayout->addWidget(fileList_);

    auto *fileBtnLayout = new QHBoxLayout;
    setDisplayBtn_ = new QPushButton(tr("Set on display"));
    deleteBtn_ = new QPushButton(tr("Delete"));
    refreshBtn_ = new QPushButton(tr("Refresh"));
    fileBtnLayout->addWidget(setDisplayBtn_);
    fileBtnLayout->addWidget(deleteBtn_);
    fileBtnLayout->addWidget(refreshBtn_);
    fileLayout->addLayout(fileBtnLayout);

    mainLayout->addWidget(fileGroup);

    connect(setDisplayBtn_, &QPushButton::clicked, this, &DisplayPage::onSetDisplayClicked);
    connect(deleteBtn_, &QPushButton::clicked, this, &DisplayPage::onDeleteClicked);
    connect(refreshBtn_, &QPushButton::clicked, this, &DisplayPage::onRefreshClicked);

    // Brightness
    auto *brightnessGroup = new QGroupBox(tr("Brightness"));
    auto *brightnessLayout = new QHBoxLayout(brightnessGroup);
    brightnessSlider_ = new QSlider(Qt::Horizontal);
    brightnessSlider_->setRange(0, 100);
    brightnessSlider_->setValue(75);
    brightnessLabel_ = new QLabel("75");
    brightnessLabel_->setMinimumWidth(30);
    brightnessLayout->addWidget(brightnessSlider_);
    brightnessLayout->addWidget(brightnessLabel_);
    mainLayout->addWidget(brightnessGroup);

    connect(brightnessSlider_, &QSlider::valueChanged, this,
            [this](int val) { brightnessLabel_->setText(QString::number(val)); });
    connect(brightnessSlider_, &QSlider::sliderReleased, this,
            [this]() { onBrightnessChanged(brightnessSlider_->value()); });

    // Ratio
    auto *optionsLayout = new QHBoxLayout;
    optionsLayout->addWidget(new QLabel(tr("Ratio:")));
    ratioCombo_ = new QComboBox;
    ratioCombo_->addItems({"2:1", "1:1"});
    optionsLayout->addWidget(ratioCombo_);
    optionsLayout->addStretch();
    mainLayout->addLayout(optionsLayout);

    mainLayout->addStretch();
}

QPixmap DisplayPage::extractThumbnail(const QString &videoPath, const QString &cachePath) {
    // Check cache first
    if (QFileInfo::exists(cachePath)) {
        return QPixmap(cachePath);
    }

    QDir().mkpath(QFileInfo(cachePath).absolutePath());

    QProcess proc;
    proc.start("ffmpeg", {"-y", "-i", videoPath,
                          "-vf", "select=eq(n\\,0),scale=384:-1",
                          "-frames:v", "1",
                          "-q:v", "5",
                          cachePath});
    proc.waitForFinished(5000);

    if (proc.exitCode() == 0 && QFileInfo::exists(cachePath)) {
        return QPixmap(cachePath);
    }
    return {};
}

void DisplayPage::loadBuiltinMedia() {
    tiles_.clear();
    QString mediaDir = builtinMediaDir();
    if (mediaDir.isEmpty()) return;

    QDir().mkpath(THUMB_CACHE_DIR);

    QDir dir(mediaDir);
    QStringList filters = {"*.mp4", "*.webm", "*.mkv", "*.avi", "*.mov",
                           "*.gif", "*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp"};
    auto entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);

    int row = 0, col = 0;
    for (const auto &entry : entries) {
        MediaEntry me;
        me.filePath = entry.absoluteFilePath();
        me.fileName = entry.completeBaseName();
        me.format = entry.suffix().toUpper();
        me.sizeBytes = entry.size();

        auto *tile = new MediaTile(me, builtinGridWidget_);
        connect(tile, &MediaTile::clicked, this, &DisplayPage::onTileClicked);

        // Extract thumbnail (cached)
        QString thumbName = entry.fileName().replace(' ', '_') + ".jpg";
        QString thumbPath = THUMB_CACHE_DIR + "/" + thumbName;
        QPixmap thumb = extractThumbnail(entry.absoluteFilePath(), thumbPath);
        tile->setThumbnail(thumb);

        builtinGrid_->addWidget(tile, row, col);
        tiles_.append(tile);

        col++;
        if (col >= GRID_COLUMNS) {
            col = 0;
            row++;
        }
    }
}

void DisplayPage::onTileClicked(MediaTile *tile) {
    tile->setSelected(!tile->isSelected());
}

void DisplayPage::onUploadBuiltinClicked() {
    QStringList selected;
    for (auto *tile : tiles_) {
        if (tile->isSelected()) {
            selected << tile->filePath();
        }
    }

    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files from the media library"));
        return;
    }

    progressBar_->setVisible(true);
    uploadBtn_->setEnabled(false);
    uploadBuiltinBtn_->setEnabled(false);

    for (const auto &path : selected) {
        deviceMgr_->uploadMedia(path);
    }
}

void DisplayPage::onUploadClicked() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select media file"), QString(),
        "Media (*.mp4 *.webm *.mkv *.avi *.mov *.gif *.jpg *.jpeg *.png *.bmp *.webp)");

    if (!path.isEmpty()) {
        progressBar_->setVisible(true);
        uploadBtn_->setEnabled(false);
        deviceMgr_->uploadMedia(path);
    }
}

void DisplayPage::onSetDisplayClicked() {
    auto selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to display"));
        return;
    }

    QStringList media;
    for (auto *item : selected) {
        media << item->text();
    }

    deviceMgr_->setScreenConfig(media, ratioCombo_->currentText());
    emit statusMessage(tr("Display configuration set"));
}

void DisplayPage::onDeleteClicked() {
    auto selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to delete"));
        return;
    }

    QStringList files;
    for (auto *item : selected) {
        files << item->text();
    }

    auto reply = QMessageBox::question(this, tr("Delete"),
                                       tr("Delete %1 file(s)?").arg(files.size()));
    if (reply == QMessageBox::Yes) {
        deviceMgr_->deleteMedia(files);
    }
}

void DisplayPage::onRefreshClicked() {
    deviceMgr_->refreshMediaList();
}

void DisplayPage::onBrightnessChanged(int value) {
    deviceMgr_->setBrightness(value);
}

void DisplayPage::onMediaListUpdated(const QStringList &files) {
    fileList_->clear();
    for (const auto &f : files) {
        fileList_->addItem(f);
    }
    emit statusMessage(tr("Files on device: %1").arg(files.size()));
}

void DisplayPage::onMediaUploaded(const QString &filename) {
    progressBar_->setVisible(false);
    uploadBtn_->setEnabled(true);
    uploadBuiltinBtn_->setEnabled(true);
    emit statusMessage(tr("Uploaded: %1").arg(filename));
    deviceMgr_->refreshMediaList();
}

void DisplayPage::onMediaDeleted() {
    emit statusMessage(tr("Files deleted"));
    deviceMgr_->refreshMediaList();
}

void DisplayPage::onUploadStatus(const QString &status) {
    emit statusMessage(status);
}

void DisplayPage::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        dropZone_->setStyleSheet(
            "QLabel {"
            "  border: 2px dashed #4CAF50;"
            "  border-radius: 8px;"
            "  padding: 20px;"
            "  color: #4CAF50;"
            "  font-size: 14px;"
            "  background: rgba(76, 175, 80, 30);"
            "}");
    }
}

void DisplayPage::dropEvent(QDropEvent *event) {
    dropZone_->setStyleSheet(
        "QLabel {"
        "  border: 2px dashed #888;"
        "  border-radius: 8px;"
        "  padding: 20px;"
        "  color: #aaa;"
        "  font-size: 14px;"
        "}");

    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            progressBar_->setVisible(true);
            uploadBtn_->setEnabled(false);
            deviceMgr_->uploadMedia(url.toLocalFile());
            break;
        }
    }
}
