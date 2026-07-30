#pragma once

#include "runtimecontract.h"

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

class MediaEditorController;
class RuntimeClient;

class DeviceMediaWorkflowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString action READ action NOTIFY stateChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool overwriteConfirmationPending
                   READ overwriteConfirmationPending
                   NOTIFY stateChanged)
    Q_PROPERTY(QString overwriteFileName READ overwriteFileName
                   NOTIFY stateChanged)

public:
    explicit DeviceMediaWorkflowController(
        RuntimeClient *runtime, MediaEditorController *editor,
        QObject *parent = nullptr);
    ~DeviceMediaWorkflowController() override;

    bool busy() const;
    QString action() const;
    QString mediaName() const;
    QString error() const;
    bool overwriteConfirmationPending() const;
    QString overwriteFileName() const;

    Q_INVOKABLE void beginEdit(
        const QString &mediaId, const QString &mediaName);
    Q_INVOKABLE void beginExport(
        const QString &mediaId, const QString &mediaName,
        const QUrl &folder, const QString &fileName);
    Q_INVOKABLE void confirmOverwrite();
    Q_INVOKABLE void cancelOverwrite();
    Q_INVOKABLE void cancelCurrent();
    Q_INVOKABLE QString suggestedExportFileName(
        const QString &remoteName) const;

    // Internal process boundary used by the Quick executable and tests.
    static int runExportHelper(const QStringList &arguments);

signals:
    void stateChanged();
    void userMessage(const QString &message, bool error);

private:
    enum class Intent {
        None,
        Edit,
        Export
    };

    void startStage(Intent intent, const QString &mediaId,
                    const QString &mediaName);
    bool resolveExportDestination(
        const QUrl &folder, const QString &fileName,
        QString *destination, QString *normalizedName,
        QString *errorMessage) const;
    void startExport();
    void finishExport(int exitCode, QProcess::ExitStatus status);
    void fail(const QString &message, bool keepEditor = false);
    void releaseArtifact();
    void clearArtifact();
    void clearPendingWorkflow();
    void updateRenewTimer();
    void onOperationUpdated(const TryxRuntimeOperationInfo &info);
    void onArtifactClaimed(
        const QString &operationId,
        const TryxRuntimeDeviceMediaArtifact &artifact);
    void onArtifactClaimFailed(
        const QString &operationId, const QString &artifactId,
        const QString &message);
    void onSaveAsNewRequested(
        const TryxRuntimeMediaTransform &transform);
    void onReplaceRequested(
        const TryxRuntimeMediaTransform &transform);
    void onRuntimeInvalidated();

    RuntimeClient *runtime_;
    MediaEditorController *editor_;
    QProcess exportProcess_;
    QTimer exportDeadline_;
    QTimer renewTimer_;
    Intent intent_ = Intent::None;
    QString mediaId_;
    QString mediaName_;
    QString error_;
    QString pendingStageOperationId_;
    QString pendingArtifactId_;
    QString pendingMutationOperationId_;
    TryxRuntimeDeviceMediaArtifact artifact_;
    QString exportFolderPath_;
    QString exportFileName_;
    QString exportDestinationPath_;
    QByteArray exportDiagnostic_;
    bool claimPending_ = false;
    bool overwriteConfirmationPending_ = false;
    bool overwriteConfirmed_ = false;
};
