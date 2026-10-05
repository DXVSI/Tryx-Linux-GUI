#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <atomic>
#include <memory>

class QFileInfo;
class PrinterProtocolTests;
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
    bool hasStartedIrreversibleOperation() const {
        return irreversibleStarted_.load(
            std::memory_order_acquire);
    }

public slots:
    void startLegacyAdbOta(
        const QString &packagePath,
        qint64 expectedSize,
        const QString &expectedSha256);
    void startRockchipLoaderUpdate(
        const QString &packagePath,
        qint64 expectedSize,
        const QString &expectedSha256);
    void cancel();

signals:
    void statusChanged(const QString &message);
    void progressChanged(int value);
    void irreversibleStarted();
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
        VerifyRemoteSha256,
        VerifyRemoteSha256Fallback,
        RebootRecovery,
        RebootLoader,
        DetectLoader,
        ReadChipInfo,
        ConfirmLoader,
        WriteStartMarker,
        UpgradeLoader,
        WriteGpt,
        WriteParameter,
        FlashPartition,
        WriteCompleteMarker,
        RebootRockchip
    };

    enum class RockchipProbeStatus {
        NoDevice,
        Unsafe,
        Valid
    };

    struct RockchipLoaderIdentity {
        RockchipProbeStatus status =
            RockchipProbeStatus::NoDevice;
        QString error;
        int deviceNumber = -1;
        quint16 vendorId = 0;
        quint16 productId = 0;
        QString locationId;
        QString mode;
        QString serial;
    };

    enum class RockchipNextAction {
        None,
        WriteStartMarker,
        UpgradeLoader,
        WriteGpt,
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
    static bool fileSha256(const QString &path, QString *sha256,
                           QString *errorMessage);
    static bool approvedPackageIdentityMatches(
        const QString &path,
        qint64 expectedSize,
        const QString &expectedSha256,
        QString *errorMessage);
    static RockchipLoaderIdentity parseRockchipLoaderIdentity(
        const QString &output,
        const QString &requiredSerial = {});
    static bool rockchipChipInfoIsRk3568(
        const QString &output,
        QString *errorMessage);
    static bool sameRockchipIdentity(
        const RockchipLoaderIdentity &left,
        const RockchipLoaderIdentity &right);

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
    QString parseRemoteSha256(const QString &output) const;
    void handleStepSuccess(const QString &output);
    void scheduleLoaderPoll(const QString &lastOutput = {});
    void confirmRockchipLoader(
        RockchipNextAction nextAction,
        const QString &status);
    void continueRockchipAction();
    void startRockchipGptWrite();
    void startNextRockchipFlashPartition();
    void startCurrentRockchipPartitionWrite();
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
    QString packageSha256_;
    qint64 packageExpectedSize_ = 0;
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
    RockchipLoaderIdentity rockchipLoaderIdentity_;
    RockchipNextAction rockchipNextAction_ =
        RockchipNextAction::None;
    std::unique_ptr<QTemporaryDir> tempDir_;
    PackageInfo package_;
    bool cancelRequested_ = false;
    bool rockchipWritesStarted_ = false;
    std::atomic_bool irreversibleStarted_{
        false};

    friend class PrinterProtocolTests;
};
