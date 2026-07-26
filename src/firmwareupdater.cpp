#include "firmwareupdater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>

namespace {

constexpr const char *kRemotePackagePath = "/sdcard/update.zip";
constexpr const char *kSupportedDevice = "cm01_se";
constexpr const char *kRockchipMachineModel = "RK3568";
constexpr const char *kRockchipMarkerAddress = "0x077ff8";
constexpr int kLoaderPollMaxAttempts = 45;
constexpr int kLoaderPollIntervalMs = 1000;

const QStringList kRockchipRequiredFiles = {
    "MiniLoaderAll.bin",
    "parameter.txt",
    "package-file",
    "uboot.img",
    "misc.img",
    "boot.img",
    "recovery.img",
    "rootfs.img",
    "oem.img",
    "userdata.img"
};

const QStringList kRockchipPartitionOrder = {
    "uboot",
    "trust",
    "misc",
    "boot",
    "recovery",
    "rootfs",
    "oem",
    "userdata"
};

QString trimmedLineValue(const QString &line) {
    const int idx = line.indexOf('=');
    if (idx < 0) {
        return {};
    }
    return line.mid(idx + 1).trimmed();
}

QString colonValue(const QString &line) {
    const int idx = line.indexOf(':');
    if (idx < 0) {
        return {};
    }
    return line.mid(idx + 1).trimmed();
}

QString humanSize(qint64 bytes) {
    const double mib = static_cast<double>(bytes) / 1024.0 / 1024.0;
    return QString::number(mib, 'f', mib >= 100.0 ? 0 : 1) + " MiB";
}

QString stepProgramName(const QString &program) {
    const QFileInfo info(program);
    return info.fileName().isEmpty() ? program : info.fileName();
}

QString executablePathIfUsable(const QString &path) {
    if (path.isEmpty()) {
        return {};
    }

    const QFileInfo info(path);
    if (info.exists() && info.isFile() && info.isExecutable()) {
        return info.absoluteFilePath();
    }
    return {};
}

}  // namespace

FirmwareUpdater::FirmwareUpdater(QObject *parent)
    : QObject(parent) {
    stepTimer_.setSingleShot(true);
    loaderPollTimer_.setSingleShot(true);
    connect(&stepTimer_, &QTimer::timeout, this, &FirmwareUpdater::onStepTimedOut);
    connect(&loaderPollTimer_, &QTimer::timeout, this, [this]() {
        if (updateMode_ != UpdateMode::RockchipLoader || currentStep_ == Step::Idle) {
            return;
        }
        ++loaderPollAttempts_;
        startProgramStep(Step::DetectLoader,
                         upgradeToolPath_,
                         {"LD"},
                         10000,
                         tr("Waiting for Rockchip loader (%1/%2)...")
                             .arg(loaderPollAttempts_)
                             .arg(kLoaderPollMaxAttempts));
    });
}

FirmwareUpdater::~FirmwareUpdater() {
    cleanupProcess();
}

QStringList FirmwareUpdater::DependencyStatus::missingNames() const {
    return missingNames(true);
}

QStringList FirmwareUpdater::DependencyStatus::missingNames(bool includeFlasher) const {
    QStringList missing;
    if (unzipPath.isEmpty()) {
        missing.append("unzip");
    }
    if (debugfsPath.isEmpty()) {
        missing.append("debugfs");
    }
    if (includeFlasher && upgradeToolPath.isEmpty()) {
        missing.append("upgrade_tool");
    }
    return missing;
}

bool FirmwareUpdater::isRunning() const {
    return currentStep_ != Step::Idle;
}

FirmwareUpdater::DependencyStatus FirmwareUpdater::dependencyStatus() const {
    return DependencyStatus{
        adbExecutable(),
        unzipExecutable(),
        debugfsExecutable(),
        upgradeToolExecutable()
    };
}

QString FirmwareUpdater::rockchipFlashingUnavailableMessage() const {
    const QString toolPath = upgradeToolExecutable();
    if (toolPath.isEmpty()) {
        return tr("Rockchip flashing requires Rockchip upgrade_tool. Install it in PATH or set TRYX_UPGRADE_TOOL to the executable path.");
    }
    return tr("Rockchip flashing will use external upgrade_tool backend: %1").arg(toolPath);
}

bool FirmwareUpdater::rockchipFlashingAvailable() const {
    return !upgradeToolExecutable().isEmpty();
}

QString FirmwareUpdater::adbExecutable() {
    return QStandardPaths::findExecutable("adb");
}

QString FirmwareUpdater::unzipExecutable() {
    return QStandardPaths::findExecutable("unzip");
}

QString FirmwareUpdater::debugfsExecutable() {
    return QStandardPaths::findExecutable("debugfs");
}

QString FirmwareUpdater::upgradeToolExecutable() {
    const QString envPath = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("TRYX_UPGRADE_TOOL"));
    const QString envExecutable = executablePathIfUsable(envPath);
    if (!envExecutable.isEmpty()) {
        return envExecutable;
    }

    const QString pathExecutable = QStandardPaths::findExecutable("upgrade_tool");
    if (!pathExecutable.isEmpty()) {
        return pathExecutable;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/upgrade_tool",
        appDir + "/tools/upgrade_tool",
        appDir + "/../tools/upgrade_tool",
        QDir::currentPath() + "/tools/upgrade_tool",
        QDir::homePath() + "/.local/bin/upgrade_tool",
        "/tmp/rockchip-upgrade-tool/Linux_Upgrade_Tool/upgrade_tool"
    };
    for (const QString &candidate : candidates) {
        const QString executable = executablePathIfUsable(candidate);
        if (!executable.isEmpty()) {
            return executable;
        }
    }
    return {};
}

QString FirmwareUpdater::combinedOutput(const QByteArray &stdOut,
                                        const QByteArray &stdErr) {
    QString output = QString::fromLocal8Bit(stdOut);
    const QString errors = QString::fromLocal8Bit(stdErr);
    if (!errors.isEmpty()) {
        if (!output.isEmpty() && !output.endsWith('\n')) {
            output += '\n';
        }
        output += errors;
    }
    return output;
}

QString FirmwareUpdater::runProcessCapture(const QString &program,
                                           const QStringList &arguments,
                                           int timeoutMs,
                                           int *exitCode) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) {
        if (exitCode) {
            *exitCode = -1;
        }
        return process.errorString();
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (exitCode) {
            *exitCode = -1;
        }
        return QObject::tr("Command timed out: %1").arg(program);
    }

    if (exitCode) {
        *exitCode = process.exitStatus() == QProcess::NormalExit
                        ? process.exitCode()
                        : -1;
    }
    return combinedOutput(process.readAllStandardOutput(),
                          process.readAllStandardError());
}

bool FirmwareUpdater::extractZipEntryToFile(const QString &unzip,
                                            const QString &packagePath,
                                            const QString &entryName,
                                            const QString &destinationPath,
                                            int timeoutMs,
                                            QString *errorMessage) {
    QFile::remove(destinationPath);

    QProcess process;
    process.setStandardOutputFile(destinationPath, QIODevice::Truncate);
    process.start(unzip, {"-p", packagePath, entryName});
    if (!process.waitForStarted(3000)) {
        if (errorMessage) {
            *errorMessage = process.errorString();
        }
        QFile::remove(destinationPath);
        return false;
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (errorMessage) {
            *errorMessage = QObject::tr("Command timed out: %1").arg(unzip);
        }
        QFile::remove(destinationPath);
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = combinedOutput(process.readAllStandardOutput(),
                                           process.readAllStandardError()).trimmed();
        }
        QFile::remove(destinationPath);
        return false;
    }

    if (QFileInfo(destinationPath).size() <= 0) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Extracted ZIP entry is empty: %1").arg(entryName);
        }
        QFile::remove(destinationPath);
        return false;
    }

    return true;
}

bool FirmwareUpdater::extractExt4FileToFile(const QString &debugfs,
                                            const QString &imagePath,
                                            const QString &entryName,
                                            const QString &destinationPath,
                                            int timeoutMs,
                                            QString *errorMessage) {
    QFile::remove(destinationPath);

    QProcess process;
    process.setStandardOutputFile(destinationPath, QIODevice::Truncate);
    process.start(debugfs, {"-R", "cat " + entryName, imagePath});
    if (!process.waitForStarted(3000)) {
        if (errorMessage) {
            *errorMessage = process.errorString();
        }
        QFile::remove(destinationPath);
        return false;
    }

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (errorMessage) {
            *errorMessage = QObject::tr("Command timed out: %1").arg(debugfs);
        }
        QFile::remove(destinationPath);
        return false;
    }

    const QString output = combinedOutput(process.readAllStandardOutput(),
                                          process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = output;
        }
        QFile::remove(destinationPath);
        return false;
    }

    if (QFileInfo(destinationPath).size() <= 0) {
        if (errorMessage) {
            *errorMessage = output.isEmpty()
                                ? QObject::tr("Extracted ZIP entry is empty: %1").arg(entryName)
                                : output;
        }
        QFile::remove(destinationPath);
        return false;
    }

    return true;
}

bool FirmwareUpdater::writeRockchipMarker(const QString &path, quint32 state,
                                          QString *errorMessage) {
    QByteArray marker(512, '\0');
    marker[0] = 'M';
    marker[1] = 'B';
    marker[2] = 'K';
    marker[3] = 'R';
    marker[4] = '\x01';
    marker[5] = '\0';
    marker[6] = '\x34';
    marker[7] = '\0';

    auto writeLe32 = [&marker](int offset, quint32 value) {
        marker[offset] = static_cast<char>(value & 0xff);
        marker[offset + 1] = static_cast<char>((value >> 8) & 0xff);
        marker[offset + 2] = static_cast<char>((value >> 16) & 0xff);
        marker[offset + 3] = static_cast<char>((value >> 24) & 0xff);
    };
    writeLe32(8, state);
    writeLe32(12, state);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    if (file.write(marker) != marker.size()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

FirmwareUpdater::PackageInfo FirmwareUpdater::validatePackage(const QString &packagePath) const {
    PackageInfo info;
    info.path = packagePath;

    const QFileInfo fileInfo(packagePath);
    if (packagePath.isEmpty()) {
        info.error = tr("No firmware package selected");
        return info;
    }
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        info.error = tr("Firmware package does not exist");
        return info;
    }
    if (fileInfo.size() <= 0) {
        info.error = tr("Firmware package is empty");
        return info;
    }
    if (fileInfo.suffix().compare("zip", Qt::CaseInsensitive) != 0) {
        info.error = tr("Firmware package must be a .zip file");
        return info;
    }

    const QString unzip = unzipExecutable();
    if (unzip.isEmpty()) {
        info.error = tr("unzip not found. Install unzip to validate firmware packages.");
        return info;
    }

    int exitCode = -1;
    const QString metadata = runProcessCapture(
        unzip,
        {"-p", packagePath, "META-INF/com/android/metadata"},
        10000,
        &exitCode);
    if (exitCode == 0 && !metadata.trimmed().isEmpty()) {
        return validateLegacyAndroidOta(packagePath, fileInfo, metadata);
    }

    return validateRockchipBundle(packagePath, fileInfo, unzip);
}

FirmwareUpdater::PackageInfo FirmwareUpdater::validateLegacyAndroidOta(
    const QString &packagePath,
    const QFileInfo &fileInfo,
    const QString &metadata) const {
    PackageInfo info;
    info.kind = PackageKind::LegacyAndroidOta;
    info.path = packagePath;
    info.sizeBytes = fileInfo.size();

    for (const QString &rawLine : metadata.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.startsWith("pre-device=")) {
            info.preDevice = trimmedLineValue(line);
        } else if (line.startsWith("post-build=")) {
            info.postBuild = trimmedLineValue(line);
        } else if (line.startsWith("post-build-incremental=")) {
            info.postBuildIncremental = trimmedLineValue(line);
        }
    }

    if (info.preDevice.isEmpty()) {
        info.error = tr("Firmware package does not declare a target device");
        return info;
    }
    if (info.preDevice != kSupportedDevice) {
        info.error = tr("Unsupported firmware target: %1").arg(info.preDevice);
        return info;
    }
    if (info.postBuild.isEmpty() || info.postBuildIncremental.isEmpty()) {
        info.error = tr("Firmware package version metadata is incomplete");
        return info;
    }

    info.valid = true;
    return info;
}

FirmwareUpdater::PackageInfo FirmwareUpdater::validateRockchipBundle(
    const QString &packagePath,
    const QFileInfo &fileInfo,
    const QString &unzip) const {
    PackageInfo info;
    info.kind = PackageKind::RockchipBundle;
    info.path = packagePath;
    info.preDevice = kSupportedDevice;
    info.sizeBytes = fileInfo.size();

    int exitCode = -1;
    const QString fileList = runProcessCapture(unzip, {"-Z", "-1", packagePath}, 10000, &exitCode);
    if (exitCode != 0 || fileList.trimmed().isEmpty()) {
        info.error = tr("Firmware package metadata not found and ZIP file list cannot be read");
        return info;
    }

    QSet<QString> entries;
    for (const QString &rawEntry : fileList.split('\n', Qt::SkipEmptyParts)) {
        entries.insert(rawEntry.trimmed());
    }
    for (const QString &required : kRockchipRequiredFiles) {
        if (!entries.contains(required)) {
            info.error = tr("Rockchip firmware package is missing %1").arg(required);
            return info;
        }
    }

    const QString parameter = runProcessCapture(unzip, {"-p", packagePath, "parameter.txt"}, 10000, &exitCode);
    if (exitCode != 0 || parameter.trimmed().isEmpty()) {
        info.error = tr("Rockchip parameter.txt cannot be read");
        return info;
    }

    for (const QString &rawLine : parameter.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.startsWith("FIRMWARE_VER:")) {
            info.firmwareVersion = colonValue(line);
        } else if (line.startsWith("MACHINE_MODEL:")) {
            info.machineModel = colonValue(line);
        }
    }

    const QRegularExpression partitionRe("@0x[0-9a-fA-F]+\\(([^)]+)\\)");
    QRegularExpressionMatchIterator partitionIt = partitionRe.globalMatch(parameter);
    while (partitionIt.hasNext()) {
        QString partition = partitionIt.next().captured(1).trimmed();
        const int optionIndex = partition.indexOf(':');
        if (optionIndex >= 0) {
            partition = partition.left(optionIndex);
        }
        if (!partition.isEmpty() && partition != "backup" && !info.partitions.contains(partition)) {
            info.partitions.append(partition);
        }
    }

    if (info.machineModel != kRockchipMachineModel) {
        info.error = tr("Unsupported Rockchip machine model: %1").arg(info.machineModel);
        return info;
    }

    const QString debugfs = debugfsExecutable();
    if (debugfs.isEmpty()) {
        info.error = tr("debugfs not found. Install e2fsprogs to inspect Rockchip rootfs images.");
        return info;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        info.error = tr("Failed to create temporary directory for firmware validation");
        return info;
    }

    QString extractError;
    const QString rootfsPath = tempDir.path() + "/rootfs.img";
    if (!extractZipEntryToFile(unzip, packagePath, "rootfs.img", rootfsPath, 120000, &extractError)) {
        info.error = tr("Failed to extract Rockchip rootfs.img: %1").arg(extractError);
        return info;
    }

    QString debugError;
    const QString panoramaBinary = tempDir.path() + "/panorama";
    if (!extractExt4FileToFile(debugfs,
                               rootfsPath,
                               "/usr/bin/panorama",
                               panoramaBinary,
                               30000,
                               &debugError)) {
        info.error = tr("Failed to inspect Rockchip product binary: %1").arg(debugError);
        return info;
    }

    if (!scanRockchipPanoramaBinary(panoramaBinary, &info.productCode, &info.appVersion)) {
        info.error = tr("Rockchip firmware product marker was not found");
        return info;
    }

    info.valid = true;
    return info;
}

bool FirmwareUpdater::scanRockchipPanoramaBinary(const QString &path,
                                                 QString *productCode,
                                                 QString *appVersion) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray data = file.readAll();
    const QString text = QString::fromLatin1(data.constData(), data.size());

    struct ProductMarker {
        QString code;
        qsizetype position;
    };
    struct VersionMarker {
        QString version;
        qsizetype start;
        qsizetype end;
    };

    QList<ProductMarker> productMarkers;
    const QStringList knownProducts = {"PASE", "PAWB", "PANO"};
    for (const QString &code : knownProducts) {
        qsizetype pos = text.indexOf(code);
        while (pos >= 0) {
            productMarkers.append({code, pos});
            pos = text.indexOf(code, pos + code.size());
        }
    }

    if (productMarkers.isEmpty()) {
        return false;
    }

    QList<VersionMarker> versionMarkers;
    const QRegularExpression versionRe("v\\d+\\.\\d+\\.\\d+\\.\\d{8}");
    QRegularExpressionMatchIterator versionIt = versionRe.globalMatch(text);
    while (versionIt.hasNext()) {
        const QRegularExpressionMatch match = versionIt.next();
        versionMarkers.append({match.captured(0), match.capturedStart(0), match.capturedEnd(0)});
    }

    QString bestProduct;
    QString bestVersion;
    qsizetype bestDistance = std::numeric_limits<qsizetype>::max();
    for (const ProductMarker &product : productMarkers) {
        for (const VersionMarker &version : versionMarkers) {
            if (version.end > product.position) {
                continue;
            }
            const qsizetype distance = product.position - version.end;
            if (distance <= 256 && distance < bestDistance) {
                bestDistance = distance;
                bestProduct = product.code;
                bestVersion = version.version;
            }
        }
    }

    if (bestProduct.isEmpty()) {
        for (const QString &preferred : knownProducts) {
            const auto it = std::find_if(productMarkers.cbegin(), productMarkers.cend(),
                                         [&preferred](const ProductMarker &marker) {
                                             return marker.code == preferred;
                                         });
            if (it != productMarkers.cend()) {
                bestProduct = preferred;
                break;
            }
        }
    }

    if (bestProduct.isEmpty()) {
        return false;
    }

    if (productCode) {
        *productCode = bestProduct;
    }
    if (appVersion) {
        *appVersion = bestVersion;
    }
    return true;
}

bool FirmwareUpdater::loadRockchipPartitionOffsets(QString *errorMessage) {
    rockchipPartitionOffsets_.clear();

    QFile parameterFile(rockchipFirmwareDir_ + "/parameter.txt");
    if (!parameterFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = parameterFile.errorString();
        }
        return false;
    }

    const QString parameter = QString::fromLocal8Bit(parameterFile.readAll());
    const QRegularExpression partitionRe("(?:0x[0-9a-fA-F]+|-)@(0x[0-9a-fA-F]+)\\(([^)]+)\\)");
    QRegularExpressionMatchIterator it = partitionRe.globalMatch(parameter);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        QString partition = match.captured(2).trimmed();
        const int optionIndex = partition.indexOf(':');
        if (optionIndex >= 0) {
            partition = partition.left(optionIndex);
        }
        if (!partition.isEmpty()) {
            rockchipPartitionOffsets_.insert(partition, match.captured(1).toLower());
        }
    }

    QStringList missing;
    for (const QString &partition : rockchipPartitions_) {
        if (!rockchipPartitionOffsets_.contains(partition)) {
            missing.append(partition);
        }
    }
    if (!missing.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("parameter.txt does not contain offsets for: %1")
                                .arg(missing.join(", "));
        }
        return false;
    }
    return true;
}

void FirmwareUpdater::startLegacyAdbOta(const QString &packagePath) {
    if (isRunning()) {
        emit statusChanged(tr("Firmware update is already running"));
        return;
    }

    package_ = validatePackage(packagePath);
    if (!package_.valid) {
        fail(package_.error);
        return;
    }
    if (package_.kind != PackageKind::LegacyAndroidOta) {
        fail(tr("Selected package is not a legacy Android OTA package"));
        return;
    }

    adbPath_ = adbExecutable();
    if (adbPath_.isEmpty()) {
        fail(tr("adb not found. Install android-tools to flash firmware."));
        return;
    }

    updateMode_ = UpdateMode::LegacyAdbOta;
    selectedSerial_.clear();
    currentBuildIncremental_.clear();
    cancelRequested_ = false;
    tempDir_.reset();

    emit progressChanged(0);
    startStep(Step::ListDevices, {"devices", "-l"}, 10000,
              tr("Searching for TRYX device over ADB..."));
}

void FirmwareUpdater::startRockchipLoaderUpdate(const QString &packagePath) {
    if (isRunning()) {
        emit statusChanged(tr("Firmware update is already running"));
        return;
    }

    package_ = validatePackage(packagePath);
    if (!package_.valid) {
        fail(package_.error);
        return;
    }
    if (package_.kind != PackageKind::RockchipBundle) {
        fail(tr("Selected package is not a Rockchip firmware bundle"));
        return;
    }
    if (package_.productCode != "PASE") {
        fail(tr("Rockchip bundle product %1 is not supported by this Panorama SE updater")
                 .arg(package_.productCode));
        return;
    }

    adbPath_ = adbExecutable();
    unzipPath_ = unzipExecutable();
    if (unzipPath_.isEmpty()) {
        fail(tr("unzip not found. Install unzip to extract firmware packages."));
        return;
    }
    upgradeToolPath_ = upgradeToolExecutable();
    if (upgradeToolPath_.isEmpty()) {
        fail(tr("upgrade_tool not found. Install Rockchip upgrade_tool in PATH or set TRYX_UPGRADE_TOOL to the executable path."));
        return;
    }

    tempDir_ = std::make_unique<QTemporaryDir>();
    if (!tempDir_ || !tempDir_->isValid()) {
        fail(tr("Failed to create temporary directory for Rockchip flashing"));
        return;
    }

    rockchipFirmwareDir_ = tempDir_->path() + "/firmware";
    if (!QDir().mkpath(rockchipFirmwareDir_)) {
        fail(tr("Failed to create temporary firmware extraction directory"));
        return;
    }

    QString markerError;
    rockchipStartMarkerPath_ = tempDir_->path() + "/bootflag_512_updating.bin";
    rockchipCompleteMarkerPath_ = tempDir_->path() + "/bootflag_512_complete.bin";
    if (!writeRockchipMarker(rockchipStartMarkerPath_, 1, &markerError) ||
        !writeRockchipMarker(rockchipCompleteMarkerPath_, 2, &markerError)) {
        fail(tr("Failed to prepare Rockchip boot marker: %1").arg(markerError));
        return;
    }

    updateMode_ = UpdateMode::RockchipLoader;
    selectedSerial_.clear();
    currentBuildIncremental_.clear();
    currentPartition_.clear();
    rockchipPartitions_.clear();
    rockchipPartitionOffsets_.clear();
    lastLoaderOutput_.clear();
    loaderPollAttempts_ = 0;
    rockchipPartitionTotal_ = 0;
    cancelRequested_ = false;

    emit progressChanged(0);
    startProgramStep(Step::ExtractRockchipPackage,
                     unzipPath_,
                     {"-q", package_.path, "-d", rockchipFirmwareDir_},
                     5 * 60 * 1000,
                     tr("Extracting Rockchip firmware package..."));
}

void FirmwareUpdater::cancel() {
    if (!isRunning()) {
        return;
    }
    if (currentStep_ == Step::RebootRecovery) {
        emit statusChanged(tr("Cannot cancel after recovery reboot command has been sent"));
        return;
    }
    if (isRockchipCancelLocked()) {
        emit statusChanged(tr("Cannot cancel after Rockchip flashing has started"));
        return;
    }

    cancelRequested_ = true;
    loaderPollTimer_.stop();
    cleanupProcess();
    const QString message = updateMode_ == UpdateMode::RockchipLoader
                                ? tr("Firmware update cancelled before Rockchip flashing started.")
                                : tr("Firmware update cancelled. A partial update.zip may remain on the device.");
    fail(message);
}

void FirmwareUpdater::startStep(Step step, const QStringList &arguments,
                                int timeoutMs, const QString &status) {
    startProgramStep(step, adbPath_, arguments, timeoutMs, status);
}

void FirmwareUpdater::startProgramStep(Step step, const QString &program,
                                       const QStringList &arguments,
                                       int timeoutMs, const QString &status) {
    cleanupProcess();
    currentStep_ = step;
    currentProgram_ = program;
    processStdOut_.clear();
    processStdErr_.clear();
    process_ = new QProcess(this);
    if (!upgradeToolPath_.isEmpty() && program == upgradeToolPath_) {
        process_->setWorkingDirectory(QFileInfo(program).absolutePath());
    }

    connect(process_, &QProcess::readyReadStandardOutput,
            this, &FirmwareUpdater::onProcessReadyRead);
    connect(process_, &QProcess::readyReadStandardError,
            this, &FirmwareUpdater::onProcessReadyRead);
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &FirmwareUpdater::onProcessFinished);
    connect(process_, &QProcess::errorOccurred,
            this, &FirmwareUpdater::onProcessError);

    emit statusChanged(status);
    process_->start(program, arguments);
    if (!process_->waitForStarted(3000)) {
        fail(tr("Failed to start %1: %2")
                 .arg(stepProgramName(program), process_->errorString()));
        return;
    }
    stepTimer_.start(timeoutMs);
}

bool FirmwareUpdater::selectAdbDevice(const QString &output, QString *errorMessage) {
    struct Candidate {
        QString serial;
        QString details;
    };

    QList<Candidate> candidates;
    const QRegularExpression lineRe("^\\s*(\\S+)\\s+device\\b(.*)$");
    for (const QString &line : output.split('\n')) {
        const QRegularExpressionMatch match = lineRe.match(line);
        if (!match.hasMatch()) {
            continue;
        }

        Candidate candidate{match.captured(1), match.captured(2)};
        const QString probe = candidate.serial + " " + candidate.details;
        if (probe.contains("TRYX", Qt::CaseInsensitive) ||
            probe.contains("product:cm01_se", Qt::CaseInsensitive) ||
            probe.contains("model:cm01_se", Qt::CaseInsensitive) ||
            probe.contains("device:cm01_se", Qt::CaseInsensitive)) {
            candidates.append(candidate);
        }
    }

    if (candidates.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("TRYX ADB device not found");
        }
        return false;
    }
    if (candidates.size() > 1) {
        if (errorMessage) {
            *errorMessage = tr("Multiple TRYX ADB devices found; disconnect extras first");
        }
        return false;
    }

    selectedSerial_ = candidates.first().serial;
    return true;
}

qint64 FirmwareUpdater::parseRemoteSize(const QString &output) const {
    const QRegularExpression numberRe("(\\d+)");
    const QRegularExpressionMatch match = numberRe.match(output);
    if (!match.hasMatch()) {
        return -1;
    }

    bool ok = false;
    const qint64 value = match.captured(1).toLongLong(&ok);
    return ok ? value : -1;
}

void FirmwareUpdater::onProcessReadyRead() {
    if (!process_) {
        return;
    }

    const QByteArray stdOut = process_->readAllStandardOutput();
    const QByteArray stdErr = process_->readAllStandardError();
    processStdOut_ += stdOut;
    processStdErr_ += stdErr;

    const QString chunk = combinedOutput(stdOut, stdErr);
    if (chunk.isEmpty()) {
        return;
    }

    QRegularExpression percentRe("(\\d{1,3})%");
    QRegularExpressionMatchIterator it = percentRe.globalMatch(chunk);
    int percent = -1;
    while (it.hasNext()) {
        bool ok = false;
        const int value = it.next().captured(1).toInt(&ok);
        if (ok) {
            percent = qBound(0, value, 100);
        }
    }
    if (percent < 0) {
        return;
    }

    if (currentStep_ == Step::PushPackage) {
        emit progressChanged(20 + (percent * 60 / 100));
    } else if (currentStep_ == Step::FlashPartition && rockchipPartitionTotal_ > 0) {
        const int completed = rockchipPartitionTotal_ - rockchipPartitions_.size() - 1;
        const int base = 42 + (qMax(0, completed) * 48 / rockchipPartitionTotal_);
        const int span = qMax(1, 48 / rockchipPartitionTotal_);
        emit progressChanged(qBound(42, base + (percent * span / 100), 90));
    }
}

void FirmwareUpdater::onProcessFinished(int exitCode,
                                        QProcess::ExitStatus exitStatus) {
    if (!process_) {
        return;
    }

    stepTimer_.stop();
    const Step finishedStep = currentStep_;
    processStdOut_ += process_->readAllStandardOutput();
    processStdErr_ += process_->readAllStandardError();
    const QString output = combinedOutput(processStdOut_, processStdErr_);
    processStdOut_.clear();
    processStdErr_.clear();
    process_->deleteLater();
    process_ = nullptr;

    if (cancelRequested_) {
        return;
    }

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        if (finishedStep == Step::VerifyRemoteSize) {
            startStep(Step::VerifyRemoteSizeFallback,
                      {"-s", selectedSerial_, "shell", "wc", "-c", kRemotePackagePath},
                      15000,
                      tr("Checking copied package size..."));
            return;
        }
        if (finishedStep == Step::RebootLoader && updateMode_ == UpdateMode::RockchipLoader) {
            emit statusChanged(tr("ADB reboot loader command returned an error; checking Rockchip loader anyway."));
            scheduleLoaderPoll(output);
            return;
        }
        if (finishedStep == Step::ListDevices && updateMode_ == UpdateMode::RockchipLoader) {
            emit statusChanged(tr("ADB device list failed; checking whether the cooler is already in Rockchip Loader mode."));
            startProgramStep(Step::DetectLoader,
                             upgradeToolPath_,
                             {"LD"},
                             10000,
                             tr("Checking Rockchip loader..."));
            return;
        }
        if (finishedStep == Step::DetectLoader && updateMode_ == UpdateMode::RockchipLoader) {
            scheduleLoaderPoll(output);
            return;
        }

        fail(tr("%1 command failed: %2")
                 .arg(stepProgramName(currentProgram_), output.trimmed()));
        return;
    }

    handleStepSuccess(output);
}

void FirmwareUpdater::onProcessError(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart && process_) {
        fail(tr("Failed to start %1: %2")
                 .arg(stepProgramName(currentProgram_), process_->errorString()));
    }
}

void FirmwareUpdater::onStepTimedOut() {
    if (currentStep_ == Step::DetectLoader && updateMode_ == UpdateMode::RockchipLoader) {
        scheduleLoaderPoll(tr("upgrade_tool LD timed out"));
        return;
    }
    fail(tr("%1 command timed out").arg(stepProgramName(currentProgram_)));
}

void FirmwareUpdater::handleStepSuccess(const QString &output) {
    switch (currentStep_) {
    case Step::ExtractRockchipPackage: {
        rockchipPartitions_.clear();
        for (const QString &partition : kRockchipPartitionOrder) {
            if (QFileInfo::exists(rockchipFirmwareDir_ + "/" + partition + ".img")) {
                rockchipPartitions_.append(partition);
            }
        }
        if (rockchipPartitions_.isEmpty()) {
            fail(tr("Extracted Rockchip package contains no flashable partitions"));
            return;
        }
        QString partitionOffsetError;
        if (!loadRockchipPartitionOffsets(&partitionOffsetError)) {
            fail(tr("Failed to read Rockchip partition offsets: %1")
                     .arg(partitionOffsetError));
            return;
        }
        rockchipPartitionTotal_ = rockchipPartitions_.size();
        emit progressChanged(5);
        if (adbPath_.isEmpty()) {
            emit statusChanged(tr("adb not found; checking whether the cooler is already in Rockchip Loader mode."));
            startProgramStep(Step::DetectLoader,
                             upgradeToolPath_,
                             {"LD"},
                             10000,
                             tr("Checking Rockchip loader..."));
            return;
        }
        startStep(Step::ListDevices, {"devices", "-l"}, 10000,
                  tr("Searching for TRYX device over ADB..."));
        return;
    }

    case Step::ListDevices: {
        QString deviceSelectionError;
        if (!selectAdbDevice(output, &deviceSelectionError)) {
            if (updateMode_ == UpdateMode::RockchipLoader) {
                emit statusChanged(tr("TRYX ADB device not found; checking whether the cooler is already in Rockchip Loader mode."));
                emit progressChanged(8);
                startProgramStep(Step::DetectLoader,
                                 upgradeToolPath_,
                                 {"LD"},
                                 10000,
                                 tr("Checking Rockchip loader..."));
                return;
            }
            fail(deviceSelectionError);
            return;
        }
        emit progressChanged(updateMode_ == UpdateMode::RockchipLoader ? 8 : 5);
        startStep(Step::GetState,
                  {"-s", selectedSerial_, "get-state"},
                  10000,
                  tr("Checking ADB device state..."));
        return;
    }

    case Step::GetState:
        if (output.trimmed() != "device") {
            fail(tr("ADB device is not ready: %1").arg(output.trimmed()));
            return;
        }
        emit progressChanged(updateMode_ == UpdateMode::RockchipLoader ? 10 : 10);
        startStep(Step::GetProductDevice,
                  {"-s", selectedSerial_, "shell", "getprop", "ro.product.device"},
                  10000,
                  tr("Checking device model..."));
        return;

    case Step::GetProductDevice: {
        const QString connectedDevice = output.trimmed();
        const QString expectedDevice = package_.preDevice.isEmpty() ? kSupportedDevice : package_.preDevice;
        if (connectedDevice != expectedDevice) {
            fail(tr("Firmware target %1 does not match connected device %2")
                     .arg(expectedDevice, connectedDevice));
            return;
        }
        emit progressChanged(updateMode_ == UpdateMode::RockchipLoader ? 12 : 15);
        startStep(Step::GetBuildIncremental,
                  {"-s", selectedSerial_, "shell", "getprop", "ro.build.version.incremental"},
                  10000,
                  tr("Checking current firmware version..."));
        return;
    }

    case Step::GetBuildIncremental:
        currentBuildIncremental_ = output.trimmed();
        if (updateMode_ == UpdateMode::RockchipLoader) {
            emit statusChanged(tr("Connected build %1; rebooting into Rockchip loader.")
                                   .arg(currentBuildIncremental_));
            emit progressChanged(15);
            startStep(Step::RebootLoader,
                      {"-s", selectedSerial_, "shell", "reboot", "loader"},
                      15000,
                      tr("Rebooting device into Rockchip loader..."));
            return;
        }

        if (currentBuildIncremental_ == package_.postBuildIncremental) {
            emit statusChanged(tr("Connected device already reports build %1; reinstalling selected package.")
                                   .arg(currentBuildIncremental_));
        }
        emit progressChanged(20);
        startStep(Step::PushPackage,
                  {"-s", selectedSerial_, "push", package_.path, kRemotePackagePath},
                  30 * 60 * 1000,
                  tr("Copying firmware package to device (%1)...")
                      .arg(humanSize(package_.sizeBytes)));
        return;

    case Step::PushPackage:
        emit progressChanged(85);
        startStep(Step::VerifyRemoteSize,
                  {"-s", selectedSerial_, "shell", "stat", "-c", "%s", kRemotePackagePath},
                  15000,
                  tr("Verifying copied package..."));
        return;

    case Step::VerifyRemoteSize:
    case Step::VerifyRemoteSizeFallback: {
        const qint64 remoteSize = parseRemoteSize(output);
        if (remoteSize != package_.sizeBytes) {
            fail(tr("Copied package size mismatch: device %1 bytes, local %2 bytes")
                     .arg(remoteSize)
                     .arg(package_.sizeBytes));
            return;
        }
        emit progressChanged(95);
        startStep(Step::RebootRecovery,
                  {"-s", selectedSerial_, "reboot", "recovery"},
                  15000,
                  tr("Rebooting device into recovery to apply firmware..."));
        return;
    }

    case Step::RebootRecovery:
        complete(tr("Firmware package copied. Device rebooted into recovery; wait until the cooler finishes updating."));
        return;

    case Step::RebootLoader:
        scheduleLoaderPoll();
        return;

    case Step::DetectLoader:
        if (!rockchipLoaderDetected(output)) {
            scheduleLoaderPoll(output);
            return;
        }
        emit progressChanged(20);
        startProgramStep(Step::WriteStartMarker,
                         upgradeToolPath_,
                         {"WL", kRockchipMarkerAddress, rockchipStartMarkerPath_},
                         60000,
                         tr("Writing Rockchip update marker..."));
        return;

    case Step::WriteStartMarker:
        emit progressChanged(25);
        startProgramStep(Step::UpgradeLoader,
                         upgradeToolPath_,
                         {"UL", rockchipFirmwareDir_ + "/MiniLoaderAll.bin", "-noreset"},
                         120000,
                         tr("Uploading Rockchip loader..."));
        return;

    case Step::UpgradeLoader:
        emit progressChanged(34);
        startRockchipGptWrite();
        return;

    case Step::WriteGpt:
        emit progressChanged(42);
        startNextRockchipFlashPartition();
        return;

    case Step::WriteParameter:
        return;

    case Step::FlashPartition:
        startNextRockchipFlashPartition();
        return;

    case Step::WriteCompleteMarker:
        emit progressChanged(95);
        startProgramStep(Step::RebootRockchip,
                         upgradeToolPath_,
                         {"RD"},
                         60000,
                         tr("Rebooting Rockchip device..."));
        return;

    case Step::RebootRockchip:
        complete(tr("Rockchip firmware flashed. Device is rebooting; wait until the cooler screen starts normally."));
        return;

    case Step::Idle:
        return;
    }
}

void FirmwareUpdater::scheduleLoaderPoll(const QString &lastOutput) {
    if (!lastOutput.trimmed().isEmpty()) {
        lastLoaderOutput_ = lastOutput.trimmed();
    }
    if (loaderPollAttempts_ >= kLoaderPollMaxAttempts) {
        const QString details = lastLoaderOutput_.isEmpty()
                                    ? tr("upgrade_tool did not report a loader device")
                                    : lastLoaderOutput_;
        fail(tr("Rockchip loader was not detected: %1").arg(details));
        return;
    }
    loaderPollTimer_.start(kLoaderPollIntervalMs);
}

void FirmwareUpdater::startRockchipGptWrite() {
    startProgramStep(Step::WriteGpt,
                     upgradeToolPath_,
                     {"DI", "-p", rockchipFirmwareDir_ + "/parameter.txt"},
                     60000,
                     tr("Writing Rockchip GPT..."));
}

void FirmwareUpdater::startNextRockchipFlashPartition() {
    while (!rockchipPartitions_.isEmpty()) {
        currentPartition_ = rockchipPartitions_.takeFirst();
        const QString imagePath = rockchipFirmwareDir_ + "/" + currentPartition_ + ".img";
        if (!QFileInfo::exists(imagePath)) {
            continue;
        }

        const int completed = rockchipPartitionTotal_ - rockchipPartitions_.size() - 1;
        const int progress = 42 + (qMax(0, completed) * 48 / qMax(1, rockchipPartitionTotal_));
        const QString offset = rockchipPartitionOffsets_.value(currentPartition_);
        if (offset.isEmpty()) {
            fail(tr("Missing Rockchip partition offset for %1").arg(currentPartition_));
            return;
        }
        emit progressChanged(qBound(42, progress, 90));
        startProgramStep(Step::FlashPartition,
                         upgradeToolPath_,
                         {"WL", offset, imagePath},
                         30 * 60 * 1000,
                         tr("Flashing Rockchip partition %1...").arg(currentPartition_));
        return;
    }

    currentPartition_.clear();
    emit progressChanged(92);
    startProgramStep(Step::WriteCompleteMarker,
                     upgradeToolPath_,
                     {"WL", kRockchipMarkerAddress, rockchipCompleteMarkerPath_},
                     60000,
                     tr("Writing Rockchip completion marker..."));
}

bool FirmwareUpdater::rockchipLoaderDetected(const QString &output) const {
    const QString probe = output.trimmed();
    if (probe.isEmpty()) {
        return false;
    }
    if (probe.contains("not found", Qt::CaseInsensitive) ||
        probe.contains("no device", Qt::CaseInsensitive)) {
        return false;
    }
    return probe.contains("DevNo", Qt::CaseInsensitive) ||
           probe.contains("Vid=", Qt::CaseInsensitive) ||
           probe.contains("Maskrom", Qt::CaseInsensitive) ||
           probe.contains("Loader", Qt::CaseInsensitive);
}

bool FirmwareUpdater::isRockchipCancelLocked() const {
    if (updateMode_ != UpdateMode::RockchipLoader) {
        return false;
    }

    switch (currentStep_) {
    case Step::WriteStartMarker:
    case Step::UpgradeLoader:
    case Step::WriteGpt:
    case Step::WriteParameter:
    case Step::FlashPartition:
    case Step::WriteCompleteMarker:
    case Step::RebootRockchip:
        return true;

    case Step::Idle:
    case Step::ExtractRockchipPackage:
    case Step::ListDevices:
    case Step::GetState:
    case Step::GetProductDevice:
    case Step::GetBuildIncremental:
    case Step::PushPackage:
    case Step::VerifyRemoteSize:
    case Step::VerifyRemoteSizeFallback:
    case Step::RebootRecovery:
    case Step::RebootLoader:
    case Step::DetectLoader:
        return false;
    }
    return false;
}

void FirmwareUpdater::fail(const QString &message) {
    stepTimer_.stop();
    loaderPollTimer_.stop();
    cleanupProcess();
    currentStep_ = Step::Idle;
    updateMode_ = UpdateMode::None;
    currentProgram_.clear();
    tempDir_.reset();
    emit statusChanged(message);
    emit finished(false, message);
}

void FirmwareUpdater::complete(const QString &message) {
    stepTimer_.stop();
    loaderPollTimer_.stop();
    cleanupProcess();
    currentStep_ = Step::Idle;
    updateMode_ = UpdateMode::None;
    currentProgram_.clear();
    tempDir_.reset();
    emit progressChanged(100);
    emit statusChanged(message);
    emit finished(true, message);
}

void FirmwareUpdater::cleanupProcess() {
    stepTimer_.stop();
    if (!process_) {
        return;
    }

    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(3000);
    }
    process_->deleteLater();
    process_ = nullptr;
    processStdOut_.clear();
    processStdErr_.clear();
}
