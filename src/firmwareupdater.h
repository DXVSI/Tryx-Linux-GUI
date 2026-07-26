#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <memory>

class QFileInfo;
class QTemporaryDir;

class FirmwareUpdater : public QObject {
    Q_OBJECT

public:
    enum class PackageKind {
        Unknown,
        LegacyAndroidOta,
        RockchipBundle
    };

    struct PackageInfo {
        bool valid = false;
        PackageKind kind = PackageKind::Unknown;
        QString error;
        QString path;
        QString preDevice;
        QString postBuild;
        QString postBuildIncremental;
        QString productCode;
        QString appVersion;
        QString firmwareVersion;
        QString machineModel;
        QStringList partitions;
        qint64 sizeBytes = 0;
    };

    struct DependencyStatus {
        QString adbPath;
        QString unzipPath;
        QString debugfsPath;
        QString upgradeToolPath;

        bool canValidate() const {
            return !unzipPath.isEmpty() &&
                   !debugfsPath.isEmpty();
        }

        bool canFlashRockchip() const {
            return !unzipPath.isEmpty() &&
                   !debugfsPath.isEmpty() &&
                   !upgradeToolPath.isEmpty();
        }

        bool canFlashLegacy() const {
            return !adbPath.isEmpty() &&
                   !unzipPath.isEmpty();
        }

        bool ready() const {
            return canFlashRockchip();
        }

        QStringList missingNames() const;
        QStringList missingNames(bool includeFlasher) const;
    };

    explicit FirmwareUpdater(QObject *parent = nullptr);
    ~FirmwareUpdater() override;

    PackageInfo validatePackage(const QString &packagePath) const;
    DependencyStatus dependencyStatus() const;
    QString rockchipFlashingUnavailableMessage() const;
    bool rockchipFlashingAvailable() const;
    bool isRunning() const;

public slots:
    void startLegacyAdbOta(const QString &packagePath);
    void startRockchipLoaderUpdate(const QString &packagePath);
    void cancel();

signals:
    void statusChanged(const QString &message);
    void progressChanged(int value);
    void finished(bool success, const QString &message);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onStepTimedOut();

private:
    enum class UpdateMode {
        None,
        LegacyAdbOta,
        RockchipLoader
    };

    enum class Step {
        Idle,
        ExtractRockchipPackage,
        ListDevices,
        GetState,
        GetProductDevice,
        GetBuildIncremental,
        PushPackage,
        VerifyRemoteSize,
        VerifyRemoteSizeFallback,
        RebootRecovery,
        RebootLoader,
        DetectLoader,
        WriteStartMarker,
        UpgradeLoader,
        WriteGpt,
        WriteParameter,
        FlashPartition,
        WriteCompleteMarker,
        RebootRockchip
    };

    static QString adbExecutable();
    static QString unzipExecutable();
    static QString debugfsExecutable();
    static QString upgradeToolExecutable();
    static QString runProcessCapture(const QString &program,
                                     const QStringList &arguments,
                                     int timeoutMs,
                                     int *exitCode);
    static QString combinedOutput(const QByteArray &stdOut,
                                  const QByteArray &stdErr);
    static bool extractZipEntryToFile(const QString &unzip,
                                      const QString &packagePath,
                                      const QString &entryName,
                                      const QString &destinationPath,
                                      int timeoutMs,
                                      QString *errorMessage);
    static bool extractExt4FileToFile(const QString &debugfs,
                                      const QString &imagePath,
                                      const QString &entryName,
                                      const QString &destinationPath,
                                      int timeoutMs,
                                      QString *errorMessage);
    static bool writeRockchipMarker(const QString &path, quint32 state,
                                    QString *errorMessage);

    PackageInfo validateLegacyAndroidOta(const QString &packagePath,
                                         const QFileInfo &fileInfo,
                                         const QString &metadata) const;
    PackageInfo validateRockchipBundle(const QString &packagePath,
                                       const QFileInfo &fileInfo,
                                       const QString &unzip) const;
    bool scanRockchipPanoramaBinary(const QString &path,
                                    QString *productCode,
                                    QString *appVersion) const;
    bool loadRockchipPartitionOffsets(QString *errorMessage);
    void startStep(Step step, const QStringList &arguments,
                   int timeoutMs, const QString &status);
    void startProgramStep(Step step, const QString &program,
                          const QStringList &arguments,
                          int timeoutMs, const QString &status);
    bool selectAdbDevice(const QString &output, QString *errorMessage);
    qint64 parseRemoteSize(const QString &output) const;
    void handleStepSuccess(const QString &output);
    void scheduleLoaderPoll(const QString &lastOutput = {});
    void startRockchipGptWrite();
    void startNextRockchipFlashPartition();
    bool rockchipLoaderDetected(const QString &output) const;
    bool isRockchipCancelLocked() const;
    void fail(const QString &message);
    void complete(const QString &message);
    void cleanupProcess();

    QProcess *process_ = nullptr;
    QTimer stepTimer_;
    QTimer loaderPollTimer_;
    Step currentStep_ = Step::Idle;
    UpdateMode updateMode_ = UpdateMode::None;
    QString currentProgram_;
    QString adbPath_;
    QString unzipPath_;
    QString upgradeToolPath_;
    QString selectedSerial_;
    QString currentBuildIncremental_;
    QString rockchipFirmwareDir_;
    QString rockchipStartMarkerPath_;
    QString rockchipCompleteMarkerPath_;
    QString currentPartition_;
    QStringList rockchipPartitions_;
    QHash<QString, QString> rockchipPartitionOffsets_;
    QString lastLoaderOutput_;
    QByteArray processStdOut_;
    QByteArray processStdErr_;
    int loaderPollAttempts_ = 0;
    int rockchipPartitionTotal_ = 0;
    std::unique_ptr<QTemporaryDir> tempDir_;
    PackageInfo package_;
    bool cancelRequested_ = false;
};
