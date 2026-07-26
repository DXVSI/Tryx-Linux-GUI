#pragma once

#include <QWidget>
#include <QListWidget>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QGridLayout>
#include <QSet>
#include <QFrame>

class DeviceManager;
struct TryxRuntimeOperationInfo;

struct MediaEntry {
    enum class Origin {
        UserFile,
        DevicePreset,
    };

    QString filePath;
    QString fileName;
    QString remoteId;
    QString format;
    quint64 sizeBytes = 0;
    Origin origin = Origin::UserFile;
};

class MediaTile : public QFrame {
    Q_OBJECT
public:
    explicit MediaTile(const MediaEntry &entry, QWidget *parent = nullptr);

    QString filePath() const { return entry_.filePath; }
    QString remoteId() const { return entry_.remoteId; }
    MediaEntry::Origin origin() const { return entry_.origin; }
    bool isSelected() const { return selected_; }
    void setSelected(bool sel);
    void setThumbnail(const QPixmap &pix);
    void setNeutralPlaceholder(const QString &text);
    QPixmap thumbnail() const { return thumb_; }

signals:
    void clicked(MediaTile *tile);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void updateStyle();

    MediaEntry entry_;
    QPixmap thumb_;
    bool selected_ = false;
    QLabel *imageLabel_;
    QLabel *nameLabel_;
    QLabel *infoLabel_;
};

class DisplayPage : public QWidget {
    Q_OBJECT
public:
    explicit DisplayPage(DeviceManager *deviceMgr, QWidget *parent = nullptr);

signals:
    void statusMessage(const QString &msg);

private slots:
    void onUploadClicked();
    void onSetDisplayClicked();
    void onDeleteClicked();
    void onRefreshClicked();
    void onBrightnessChanged(int value);
    void onMediaListUpdated(const QStringList &files);
    void onMediaUploaded(const QString &filename);
    void onMediaDeleted();
    void onUploadStatus(const QString &status);
    void onOperationChanged(const TryxRuntimeOperationInfo &info,
                            quint64 revision);
    void onRetryClicked();
    void onCancelClicked();

private:
    void setupUi();
    void setUploadBusy(bool busy);

    DeviceManager *deviceMgr_;
    QListWidget *fileList_;
    QSlider *brightnessSlider_;
    QLabel *brightnessLabel_;
    QComboBox *ratioCombo_;
    QPushButton *uploadBtn_;
    QPushButton *setDisplayBtn_;
    QPushButton *deleteBtn_;
    QPushButton *refreshBtn_;
    QPushButton *retryBtn_;
    QPushButton *cancelBtn_;
    QLabel *dropZone_;
    QProgressBar *progressBar_;
    bool uploadBusy_ = false;
    QString activeOperationId_;
    QString retryOperationId_;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
