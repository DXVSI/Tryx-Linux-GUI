#pragma once

#include "runtimecontract.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <atomic>

class QProcess;

class PrinterMediaPreparer : public QObject {
    Q_OBJECT

#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif

public:
    explicit PrinterMediaPreparer(QObject *parent = nullptr);
    ~PrinterMediaPreparer() override;
    void cancelRetryValidation(const QString &validationId);
    void clearRetryValidationCancellation(const QString &validationId);
    void requestOperationCancellation(const QString &operationId);
    void requestGenerationCancellation(quint64 currentGeneration);

public slots:
    void analyzeSource(const QString &operationId,
                       const QString &localPath,
                       quint64 generation,
                       const TryxRuntimeMediaTransform &transform =
                           TryxRuntimeMediaTransform{},
                       quint16 productId = 0x1021);
    void prepare(const QString &operationId, const QString &devicePath,
                 const QString &localPath,
                 const QString &expectedSourceSha256,
                 quint64 generation,
                 const TryxRuntimeMediaTransform &transform =
                     TryxRuntimeMediaTransform{},
                 quint16 productId = 0x1021);
    void prepareRecovered(const QString &operationId,
                          const QString &devicePath,
                          const QString &localPath,
                          const QString &expectedSourceSha256,
                          quint64 generation,
                          const TryxRuntimeMediaTransform &transform =
                              TryxRuntimeMediaTransform{},
                          quint16 productId = 0x1021);
    void cancelStale(quint64 currentGeneration);
    void cancelOperation(const QString &operationId);
    void validateRetryCache(const QString &validationId,
                            const QString &preparedPath,
                            const QString &expectedSha256);
    void releasePreparedFile(const QString &uploadPath);
    void shutdown();

signals:
    void sourceAnalyzed(const QString &operationId,
                        const QString &localPath,
                        const QString &contentSha256,
                        qint64 sourceSize,
                        const QString &conversionProfile,
                        quint64 generation);
    void progress(const QString &operationId, const QString &message,
                  quint64 generation);
    void prepared(const QString &operationId, const QString &devicePath,
                  const QString &sourcePath, const QString &uploadPath,
                  const QString &remoteName, const QString &preparedSha256,
                  const QString &stagedThumbnailPath,
                  const QString &stagedThumbnailSha256,
                  quint64 generation);
    void failed(const QString &operationId, const QString &message,
                quint64 generation);
    void retryCacheValidated(const QString &validationId, bool valid,
                             bool cancelled, const QString &message);

private:
    enum class PreparationPhase {
        Idle,
        Media,
        Thumbnail,
        FrameCount
    };

    void startPreparation(const QString &operationId,
                          const QString &devicePath,
                          const QString &localPath,
                          const QString &expectedSourceSha256,
                          quint64 generation,
                          const TryxRuntimeMediaTransform &transform =
                              TryxRuntimeMediaTransform{},
                          bool recoveredVideo = false,
                          quint16 productId = 0x1021);
    void finishPreparation(int exitCode, bool normalExit);
    void finishMediaPreparation(int exitCode, bool normalExit);
    void finishThumbnailPreparation(int exitCode, bool normalExit);
    void startTurrisFrameCountPreparation(
        const QString &thumbnailSha256);
    void finishTurrisFrameCountPreparation(int exitCode, bool normalExit);
    void completePreparation(const QString &thumbnailSha256);
    void failPreparation(const QString &message, bool cancelled);
    void resetPreparationState();
    void startPendingIfAvailable();

    QProcess *process_;
    QTimer *processDeadlineTimer_;
    QDeadlineTimer mediaPreparationDeadline_;
    QString operationId_;
    QString devicePath_;
    QString sourcePath_;
    QString uploadPath_;
    QString rawMediaPath_;
    QString remoteName_;
    QString stagedThumbnailTempPath_;
    QString stagedThumbnailPath_;
    QString stagedThumbnailSha256_;
    QString preparedSha256_;
    QString expectedSourceSha256_;
    TryxRuntimeMediaTransform transform_;
    quint16 productId_ = 0x1021;
    quint32 turrisMediaKind_ = 0;
    bool recoveredVideo_ = false;
    quint64 generation_ = 0;
    QByteArray processOutput_;
    PreparationPhase phase_ = PreparationPhase::Idle;
    bool active_ = false;
    bool cancelling_ = false;
    bool preparationTimedOut_ = false;
    bool shuttingDown_ = false;
    bool hasPending_ = false;
    QString pendingOperationId_;
    QString pendingDevicePath_;
    QString pendingLocalPath_;
    QString pendingExpectedSourceSha256_;
    TryxRuntimeMediaTransform pendingTransform_;
    bool pendingRecoveredVideo_ = false;
    quint16 pendingProductId_ = 0x1021;
    quint64 pendingGeneration_ = 0;
    QSet<QString> deliveredPaths_;
    mutable QMutex retryValidationMutex_;
    QSet<QString> cancelledRetryValidations_;
    mutable QMutex preparationCancellationMutex_;
    QSet<QString> cancelledPreparationOperations_;
    std::atomic<quint64> preparationGenerationGate_{0};
};
