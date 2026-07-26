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
#include <QPixmap>
#include <QMouseEvent>
#include <QSignalBlocker>

static const int TILE_WIDTH = 200;
static const int TILE_IMG_HEIGHT = 100;

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

    const double sizeMB =
        static_cast<double>(entry_.sizeBytes) / (1024.0 * 1024.0);
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

void MediaTile::setNeutralPlaceholder(const QString &text) {
    thumb_ = {};
    imageLabel_->setPixmap({});
    imageLabel_->setText(text);
    imageLabel_->setWordWrap(true);
    imageLabel_->setStyleSheet(
        "background: #20202c;"
        "border: 1px solid #3d3d4d;"
        "border-radius: 4px;"
        "color: #9a9aaa;"
        "padding: 8px;");
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
    connect(deviceMgr_, &DeviceManager::operationChanged, this,
            &DisplayPage::onOperationChanged);
    connect(deviceMgr_, &DeviceManager::operationSnapshotUpdated, this,
            [this](const TryxRuntimeOperationsSnapshot &snapshot) {
                if (snapshot.activeOperationId.isEmpty()) {
                    return;
                }
                activeOperationId_ = snapshot.activeOperationId;
                onOperationChanged(
                    deviceMgr_->operationInfo(activeOperationId_),
                    snapshot.revision);
            });
    connect(deviceMgr_, &DeviceManager::deviceError, this,
            [this](const QString &message) {
                refreshBtn_->setEnabled(!uploadBusy_);
                emit statusMessage(message);
            });
    connect(deviceMgr_, &DeviceManager::brightnessChanged, this,
            [this](int val) {
                const QSignalBlocker blocker(brightnessSlider_);
                brightnessSlider_->setValue(val);
                brightnessLabel_->setText(QString::number(val));
            });
}

void DisplayPage::setupUi() {
    setAcceptDrops(true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

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
    retryBtn_ = new QPushButton(tr("Retry transfer"));
    retryBtn_->setVisible(false);
    uploadLayout->addWidget(retryBtn_);
    cancelBtn_ = new QPushButton(tr("Cancel"));
    cancelBtn_->setVisible(false);
    uploadLayout->addWidget(cancelBtn_);
    mainLayout->addLayout(uploadLayout);

    connect(uploadBtn_, &QPushButton::clicked, this, &DisplayPage::onUploadClicked);
    connect(retryBtn_, &QPushButton::clicked, this,
            &DisplayPage::onRetryClicked);
    connect(cancelBtn_, &QPushButton::clicked, this,
            &DisplayPage::onCancelClicked);

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
    brightnessSlider_->setValue(0);
    brightnessLabel_ = new QLabel(QStringLiteral("--"));
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

void DisplayPage::onUploadClicked() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select media file"), QString(),
        "Media (*.mp4 *.webm *.mkv *.avi *.mov *.gif *.jpg *.jpeg *.png *.bmp *.webp)");

    if (!path.isEmpty()) {
        setUploadBusy(true);
        if (deviceMgr_->isPrinterClassDevicePresent()) {
            activeOperationId_ =
                deviceMgr_->queueUploadOperation(QString(), path, false);
        } else {
            deviceMgr_->uploadMedia(path);
        }
    }
}

void DisplayPage::onSetDisplayClicked() {
    auto selected = fileList_->selectedItems();
    if (selected.isEmpty()) {
        emit statusMessage(tr("Select files to display"));
        return;
    }
    if (deviceMgr_->isPrinterClassDevicePresent() && selected.size() > 1) {
        emit statusMessage(tr("Only one media file can be applied on printer-class firmware yet."));
        return;
    }

    QStringList media;
    for (auto *item : selected) {
        media << item->text();
    }

    if (deviceMgr_->isPrinterClassDevicePresent()) {
        TryxRuntimeApplyRequest request;
        request.media = media;
        request.ratio = ratioCombo_->currentText();
        request.screenMode = QStringLiteral("Full Screen");
        request.playMode = QStringLiteral("Single");
        activeOperationId_ =
            deviceMgr_->queueApplyOperation(QString(), request);
        setUploadBusy(true);
    } else {
        deviceMgr_->setScreenConfig(media, ratioCombo_->currentText());
        emit statusMessage(tr("Display configuration set"));
    }
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

    if (deviceMgr_->isPrinterClassDevicePresent()) {
        if (files.size() != 1) {
            emit statusMessage(
                tr("Select exactly one PASE media file to delete"));
            return;
        }
        TryxRuntimeMediaEntry matched;
        bool found = false;
        for (const TryxRuntimeMediaEntry &entry :
             deviceMgr_->mediaCatalogSnapshot().entries) {
            if (entry.name == files.constFirst()) {
                matched = entry;
                found = true;
                break;
            }
        }
        if (!found || !matched.deleteAllowed) {
            emit statusMessage(
                matched.deleteBlockReason.isEmpty()
                    ? tr("This PASE media file cannot be deleted")
                    : tr("This PASE media file cannot be deleted: %1")
                          .arg(matched.deleteBlockReason));
            return;
        }
        const auto reply = QMessageBox::question(
            this, tr("Delete"),
            tr("Delete this file from PASE?\n\nName: %1\nSize: %2 bytes\n\nThe operation cannot be undone.")
                .arg(matched.name)
                .arg(matched.size),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            activeOperationId_ = deviceMgr_->queueDeleteMediaOperation(
                QString(), QStringList{matched.name});
            setUploadBusy(true);
        }
        return;
    }

    const auto reply = QMessageBox::question(
        this, tr("Delete"), tr("Delete %1 file(s)?").arg(files.size()));
    if (reply == QMessageBox::Yes) {
        deviceMgr_->deleteMedia(files);
    }
}

void DisplayPage::onRefreshClicked() {
    if (!refreshBtn_->isEnabled()) {
        return;
    }
    refreshBtn_->setEnabled(false);
    deviceMgr_->refreshMediaList();
}

void DisplayPage::onBrightnessChanged(int value) {
    deviceMgr_->setBrightness(value);
}

void DisplayPage::onMediaListUpdated(const QStringList &files) {
    refreshBtn_->setEnabled(!uploadBusy_);
    fileList_->clear();
    for (const auto &f : files) {
        fileList_->addItem(f);
    }
    emit statusMessage(tr("Files on device: %1").arg(files.size()));
}

void DisplayPage::onMediaUploaded(const QString &filename) {
    if (deviceMgr_->isPrinterClassDevicePresent()) {
        return;
    }
    setUploadBusy(false);
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

void DisplayPage::onOperationChanged(
    const TryxRuntimeOperationInfo &info, quint64 revision) {
    Q_UNUSED(revision);
    const bool terminal = info.state == QStringLiteral("Succeeded") ||
                          info.state == QStringLiteral("Failed") ||
                          info.state == QStringLiteral("Cancelled") ||
                          info.state == QStringLiteral("RetryAvailable");
    if (info.id != activeOperationId_ &&
        info.state != QStringLiteral("RetryAvailable")) {
        return;
    }

    if (!terminal) {
        setUploadBusy(true);
        retryBtn_->setVisible(false);
        cancelBtn_->setVisible(true);
        if (info.total > 0) {
            progressBar_->setRange(0, 100);
            const int percent = static_cast<int>(qBound<qint64>(
                qint64(0), (info.completed * 100) / info.total,
                qint64(100)));
            progressBar_->setValue(percent);
        } else {
            progressBar_->setRange(0, 0);
        }
        if (!info.message.isEmpty()) {
            emit statusMessage(info.message);
        }
        return;
    }

    if (info.state == QStringLiteral("RetryAvailable")) {
        const bool preparedRetry =
            info.retryMode == QStringLiteral("PreparedMedia");
        retryOperationId_ = preparedRetry ? info.id : QString();
        retryBtn_->setVisible(preparedRetry);
        cancelBtn_->setVisible(false);
        setUploadBusy(false);
        retryBtn_->setVisible(preparedRetry);
        QString retryStatus = info.message.isEmpty()
            ? tr("Prepared media is available for manual retry")
            : info.message;
        if (!info.primaryErrorMessage.trimmed().isEmpty()) {
            retryStatus += QLatin1Char('\n') +
                tr("Initial transfer error: %1")
                    .arg(info.primaryErrorMessage.trimmed());
        }
        if (info.confirmedBytes > 0 && info.total > 0) {
            retryStatus += QLatin1Char('\n') +
                tr("Confirmed in the previous attempt: %1 of %2 bytes")
                    .arg(info.confirmedBytes)
                    .arg(info.total);
        }
        emit statusMessage(retryStatus);
    } else {
        setUploadBusy(false);
        cancelBtn_->setVisible(false);
        if (info.state == QStringLiteral("Succeeded")) {
            emit statusMessage(
                info.kind == QStringLiteral("DeleteMedia")
                    ? tr("Media file deleted and verified")
                    : info.kind.contains(QStringLiteral("Apply"))
                    ? tr("Display media applied")
                    : tr("Media upload completed and verified"));
        } else if (!info.message.isEmpty()) {
            emit statusMessage(info.message);
        }
    }
    if (info.id == activeOperationId_) {
        activeOperationId_.clear();
    }
}

void DisplayPage::onRetryClicked() {
    if (retryOperationId_.isEmpty()) {
        return;
    }
    activeOperationId_ =
        deviceMgr_->retryOperation(retryOperationId_, QString());
    retryBtn_->setVisible(false);
    setUploadBusy(true);
}

void DisplayPage::onCancelClicked() {
    if (!activeOperationId_.isEmpty()) {
        deviceMgr_->cancelOperation(activeOperationId_);
    }
}

void DisplayPage::setUploadBusy(bool busy) {
    uploadBusy_ = busy;
    progressBar_->setVisible(busy);
    uploadBtn_->setEnabled(!busy);
    refreshBtn_->setEnabled(!busy);
    setDisplayBtn_->setEnabled(!busy);
    if (!busy) {
        progressBar_->setRange(0, 0);
    }
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
            setUploadBusy(true);
            if (deviceMgr_->isPrinterClassDevicePresent()) {
                activeOperationId_ = deviceMgr_->queueUploadOperation(
                    QString(), url.toLocalFile(), false);
            } else {
                deviceMgr_->uploadMedia(url.toLocalFile());
            }
            break;
        }
    }
}
