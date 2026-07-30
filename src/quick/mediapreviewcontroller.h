#pragma once

#include "runtimecontract.h"

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

class QuickClientTests;

class MediaPreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    enum class SourceKind {
        None,
        InboxSnapshot,
        RecoveredVideo
    };
    Q_ENUM(SourceKind)

    explicit MediaPreviewController(QObject *parent = nullptr);
    ~MediaPreviewController() override;

    bool busy() const;
    bool ready() const;
    QUrl previewUrl() const;
    QString sourcePath() const;
    QString error() const;

    void load(const QUrl &source);
    void loadRecoveredVideo(
        const TryxRuntimeDeviceMediaArtifact &artifact);
    void setTransform(const TryxRuntimeMediaTransform &transform);
    Q_INVOKABLE void cancel();

    // The runtime atomically claims the inbox file before acknowledging an
    // upload. While a request is in flight, the GUI must leave the source in
    // place even if it exits.
    bool protectStagedSource();
    void restoreStagedSourceOwnership();
    void releaseStagedSource();

    static QString previewFilter(
        const TryxRuntimeMediaTransform &transform);
    // Internal process boundary used by the GUI executable and its tests.
    static int runStageCopyHelper(const QStringList &arguments);

signals:
    void stateChanged();

private:
    friend class QuickClientTests;

    struct StageResult {
        QString stagedPath;
        QString error;
    };
    struct RecoveredValidationResult {
        QString sourcePath;
        QString error;
    };

    static StageResult copySourceSnapshot(
        const QString &sourcePath,
        const QString &finalPath,
        qint64 expectedSize,
        const QDateTime &expectedModified);
    static bool ensurePrivateDirectory(
        const QString &path, bool create,
        QString *errorMessage);
    static bool ensurePrivateDirectoryTree(
        const QString &leafPath, QString *errorMessage);
    static bool privateDirectoryTreeIsSafe(
        const QString &leafPath);
    static bool isSupportedSuffix(const QString &suffix);
    static bool isManagedInboxPath(const QString &path);
    static bool isManagedRecoveredArtifactPath(
        const QString &path);
    static RecoveredValidationResult validateRecoveredArtifact(
        const TryxRuntimeDeviceMediaArtifact &artifact);
    static bool stageHelperDrainInProgress();
    static void cleanupStaleArtifacts();

    void startStaging(const QString &sourcePath,
                      const QString &suffix,
                      qint64 expectedSize,
                      const QDateTime &expectedModified);
    void finishStagingProcess(
        QProcess *process, quint64 generation,
        const QString &sourcePath,
        const QString &finalPath,
        qint64 expectedSize,
        const QDateTime &expectedModified,
        int exitCode, QProcess::ExitStatus status);
    void failStagingProcess(
        QProcess *process, quint64 generation,
        const QString &message);
    void abortStagingProcess();
    void schedulePreview();
    void startPreview();
    void finishProcess(int exitCode, QProcess::ExitStatus status);
    void finishPreview();
    void fail(const QString &message);
    void stopProcess();
    void stopPreviewWork();
    void resetVisibleState();
    void removeOwnedStagedSource();
    void removePendingStageArtifacts();
    void removePreviewArtifact(const QString &path);

    QProcess process_;
    QTimer processDeadline_;
    QTimer stagingDeadline_;
    QTimer renderDebounce_;
    QByteArray diagnostic_;
    QString pendingStagePath_;
    QString stagedPath_;
    QString pendingOutputPath_;
    QString currentOutputPath_;
    QUrl previewUrl_;
    QString error_;
    TryxRuntimeMediaTransform transform_;
    QProcess *stageProcess_ = nullptr;
    QString stageCopyProgram_;
    QByteArray stagingDiagnostic_;
    quint64 stageGeneration_ = 0;
    quint64 activeStageGeneration_ = 0;
    quint64 requestedRenderGeneration_ = 0;
    quint64 activeRenderGeneration_ = 0;
    bool staging_ = false;
    bool recoveredValidation_ = false;
    bool ready_ = false;
    bool sourceProtected_ = false;
    quint64 recoveredValidationGeneration_ = 0;
    SourceKind sourceKind_ = SourceKind::None;
};
