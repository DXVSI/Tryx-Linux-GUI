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
    QString filePath;
    QString fileName;
    QString format;
    qint64 sizeBytes;
};

class MediaTile : public QFrame {
    Q_OBJECT
public:
    explicit MediaTile(const MediaEntry &entry, QWidget *parent = nullptr);

    QString filePath() const { return entry_.filePath; }
    bool isSelected() const { return selected_; }
    void setSelected(bool sel);
    void setThumbnail(const QPixmap &pix);
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

    static QString builtinMediaDir();

signals:
    void statusMessage(const QString &msg);

private slots:
    void onUploadClicked();
    void onUploadBuiltinClicked();
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
    void onTileClicked(MediaTile *tile);

private:
    void setupUi();
    void loadBuiltinMedia();
    QPixmap extractThumbnail(const QString &videoPath, const QString &cachePath);
    void setUploadBusy(bool busy);

    DeviceManager *deviceMgr_;
    QListWidget *fileList_;
    QSlider *brightnessSlider_;
    QLabel *brightnessLabel_;
    QComboBox *ratioCombo_;
    QPushButton *uploadBtn_;
    QPushButton *uploadBuiltinBtn_;
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

    QScrollArea *builtinScrollArea_;
    QWidget *builtinGridWidget_;
    QGridLayout *builtinGrid_;
    QList<MediaTile *> tiles_;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
