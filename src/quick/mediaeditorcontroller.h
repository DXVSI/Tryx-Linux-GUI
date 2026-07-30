#pragma once

#include "mediapreviewcontroller.h"
#include "runtimecontract.h"

#include <QObject>
#include <QUrl>
#include <QVariantList>

class RuntimeClient;

class MediaEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY previewChanged)
    Q_PROPERTY(bool submissionPending READ submissionPending
                   NOTIFY previewChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY previewChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY previewChanged)
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY previewChanged)
    Q_PROPERTY(bool recoveredDeviceCopy READ recoveredDeviceCopy
                   NOTIFY previewChanged)
    Q_PROPERTY(QString originalMediaName READ originalMediaName
                   NOTIFY previewChanged)
    Q_PROPERTY(bool replaceAllowed READ replaceAllowed
                   NOTIFY previewChanged)
    Q_PROPERTY(QString replaceBlockReason READ replaceBlockReason
                   NOTIFY previewChanged)
    Q_PROPERTY(QString submissionAction READ submissionAction
                   NOTIFY previewChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(QString error READ error NOTIFY previewChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY transformChanged)
    Q_PROPERTY(int zoomPercent READ zoomPercent WRITE setZoomPercent
                   NOTIFY transformChanged)
    Q_PROPERTY(int focusX READ focusX WRITE setFocusX
                   NOTIFY transformChanged)
    Q_PROPERTY(int focusY READ focusY WRITE setFocusY
                   NOTIFY transformChanged)
    Q_PROPERTY(int rotation READ rotation WRITE setRotation
                   NOTIFY transformChanged)
    Q_PROPERTY(QString backgroundColor READ backgroundColor
                   WRITE setBackgroundColor NOTIFY transformChanged)
    Q_PROPERTY(QUrl homeFolder READ homeFolder CONSTANT)

public:
    explicit MediaEditorController(RuntimeClient *runtime,
                                   QObject *parent = nullptr);

    bool isOpen() const;
    bool busy() const;
    bool submissionPending() const;
    bool ready() const;
    QString sourceName() const;
    QString sourceKind() const;
    bool recoveredDeviceCopy() const;
    QString originalMediaName() const;
    bool replaceAllowed() const;
    QString replaceBlockReason() const;
    QString submissionAction() const;
    QUrl previewUrl() const;
    QString error() const;
    QString mode() const;
    int zoomPercent() const;
    int focusX() const;
    int focusY() const;
    int rotation() const;
    QString backgroundColor() const;
    QUrl homeFolder() const;
    TryxRuntimeMediaTransform transform() const;
    void beginRecoveredVideo(
        const TryxRuntimeDeviceMediaArtifact &artifact);
    void beginRecoveredSubmission(
        const QString &operationId, const QString &action);
    void finishRecoveredSubmission(
        const QString &operationId, bool success,
        const QString &message);

    Q_INVOKABLE void begin(const QUrl &source);
    Q_INVOKABLE void beginDropped(const QVariantList &sources);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void submit();
    Q_INVOKABLE void submitSaveAsNew();
    Q_INVOKABLE void submitReplace();

public slots:
    void setMode(const QString &mode);
    void setZoomPercent(int value);
    void setFocusX(int value);
    void setFocusY(int value);
    void setRotation(int value);
    void setBackgroundColor(const QString &value);

signals:
    void openChanged();
    void previewChanged();
    void transformChanged();
    void submitted();
    void cancelled();
    void recoveredSaveAsNewRequested(
        const TryxRuntimeMediaTransform &transform);
    void recoveredReplaceRequested(
        const TryxRuntimeMediaTransform &transform);
    void recoveredClosed();

private:
    RuntimeClient *runtime_;
    MediaPreviewController preview_;
    bool open_ = false;
    QString sourceName_;
    QString localPath_;
    QString editorError_;
    QString pendingOperationId_;
    TryxRuntimeDeviceMediaArtifact recoveredArtifact_;
    QString recoveredSubmissionAction_;
    QString mode_ = QStringLiteral("Fit");
    int zoomPercent_ = 100;
    int focusX_ = 5000;
    int focusY_ = 5000;
    int rotation_ = 0;
    QString backgroundColor_ = QStringLiteral("#000000");
};
