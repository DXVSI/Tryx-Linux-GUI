#include "deviceworker.h"
#include "deviceworkersessioncontext_p.h"
#include "legacydevicesession.h"
#include "printerclasssession.h"
#include <sys/eventfd.h>
#include <unistd.h>

DeviceWorker::DeviceWorker(QObject *parent)
    : QObject(parent),
      printerCancellationFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      printerOperationCancellationFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      printerSession_(new PrinterClassSession(
          *this, DeviceWorkerSessionContext(*this))),
      legacySession_(new LegacyDeviceSession(*this, printerSession_->telemetry())) {
}

DeviceWorker::~DeviceWorker() {
    // Stop printer notifications first, as before. Destroy the legacy borrower
    // before its telemetry provider, then both sessions before the gate fds.
    printerSession_->stopPrinterSession();
    delete legacySession_;
    delete printerSession_;
    if (printerCancellationFd_ >= 0) {
        ::close(printerCancellationFd_);
        printerCancellationFd_ = -1;
    }
    if (printerOperationCancellationFd_ >= 0) {
        ::close(printerOperationCancellationFd_);
        printerOperationCancellationFd_ = -1;
    }
}

void DeviceWorker::updatePrinterGenerationGate(quint64 generation,
                                                bool endpointReady) {
    printerGenerationGate_.store(generation, std::memory_order_release);
    printerEndpointReady_.store(endpointReady, std::memory_order_release);
    if (printerCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored = ::write(printerCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
    if (printerOperationCancellationFd_ >= 0) {
        const uint64_t value = 1;
        const ssize_t ignored =
            ::write(printerOperationCancellationFd_, &value, sizeof(value));
        Q_UNUSED(ignored);
    }
}

void DeviceWorker::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerSession_->setPrinterOverlayLeaseMode(mode);
}

void DeviceWorker::publishPresentationPreferences(
    const TryxRuntimePresentationPreferencesV1 &preferences) {
    if (!tryxPresentationPreferencesAreValid(preferences)) {
        return;
    }
    unsigned int encoded = 0;
    if (preferences.temperatureUnit == QStringLiteral("Fahrenheit")) {
        encoded |= 1U;
    }
    if (preferences.timeFormat == QStringLiteral("12H")) {
        encoded |= 2U;
    }
    publishedPresentationPreferences_.store(
        encoded, std::memory_order_release);
}

void DeviceWorker::applyPublishedPresentationPreferences() {
    printerSession_->applyPublishedPresentationPreferences();
}

void DeviceWorker::cancelPrinterOperation(const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&printerOperationCancellationMutex_);
        cancelledPrinterOperationIds_.insert(operationId);
    }
    if (printerOperationCancellationFd_ < 0) {
        return;
    }
    const uint64_t value = 1;
    const ssize_t ignored =
        ::write(printerOperationCancellationFd_, &value, sizeof(value));
    Q_UNUSED(ignored);
}

void DeviceWorker::clearPrinterOperationCancellation(
    const QString &operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    QMutexLocker locker(&printerOperationCancellationMutex_);
    cancelledPrinterOperationIds_.remove(operationId);
}

#ifdef TRYX_PROTOCOL_TESTING
void DeviceWorker::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    printerSession_->adoptPrinterFileDescriptorForTesting(fd, devicePath);
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
bool DeviceWorker::printerSessionActiveForTesting() const {
    return printerSession_->printerSessionActiveForTesting();
}
#endif

void DeviceWorker::connectDevice(const QString &port) {
    legacySession_->connectDevice(port);
}

void DeviceWorker::disconnectDevice() {
    legacySession_->disconnectDevice();
}

void DeviceWorker::doHandshake() {
    legacySession_->doHandshake();
}

void DeviceWorker::setBrightness(int value) {
    legacySession_->setBrightness(value);
}

void DeviceWorker::setScreenConfig(const QStringList &media, const QString &ratio,
                                   const QString &screenMode, const QString &playMode,
                                   const QStringList &sysinfoLabels,
                                   const QString &settingsPosition,
                                   const QString &settingsColor,
                                   const QString &settingsAlign,
                                   const QStringList &settingsBadges,
                                   int filterOpacity,
                                   const QString &presetId,
                                   const QStringList &sysinfoLabels2,
                                   const QStringList &settingsBadges2,
                                   bool waterfallMode) {
    legacySession_->setScreenConfig(media, ratio, screenMode, playMode, sysinfoLabels, settingsPosition, settingsColor, settingsAlign, settingsBadges, filterOpacity, presetId, sysinfoLabels2, settingsBadges2, waterfallMode);
}

void DeviceWorker::sendSysinfo(const QStringList &labels, const QStringList &values,
                               const QStringList &units) {
    legacySession_->sendSysinfo(labels, values, units);
}

void DeviceWorker::sendLegacyMetrics() {
    legacySession_->sendLegacyMetrics();
}

void DeviceWorker::deleteMedia(const QStringList &files) {
    legacySession_->deleteMedia(files);
}

void DeviceWorker::uploadMedia(const QString &localPath) {
    legacySession_->uploadMedia(localPath);
}

void DeviceWorker::uploadPreparedPrinterMedia(const QString &devicePath,
                                              const QString &uploadPath,
                                              const QString &remoteName,
                                              const QString &expectedSha256,
                                              const QString &operationId,
                                              quint64 generation) {
    printerSession_->uploadPreparedPrinterMedia(devicePath, uploadPath, remoteName, expectedSha256, operationId, generation);
}

void DeviceWorker::refreshMediaList() {
    legacySession_->refreshMediaList();
}

void DeviceWorker::configurePrinterDevice(const QString &devicePath,
                                          const QString &deviceSerial,
                                          quint16 productId,
                                          quint64 generation) {
    printerSession_->configurePrinterDevice(devicePath, deviceSerial, productId, generation);
}

void DeviceWorker::restorePrinterOverlay(
                                         const PrinterProtocol::PaseOverlayConfig &overlay,
                                         quint64 generation) {
    printerSession_->restorePrinterOverlay(overlay, generation);
}

void DeviceWorker::beginPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    printerSession_->beginPrinterForegroundOperation(operationId, generation);
}

void DeviceWorker::endPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    printerSession_->endPrinterForegroundOperation(operationId, generation);
}

void DeviceWorker::clearPrinterDevice(quint64 generation) {
    printerSession_->clearPrinterDevice(generation);
}

void DeviceWorker::quiesceDeviceTransports(quint64 generation) {
    legacySession_->quiesce();
    printerSession_->quiesce(generation);
}

void DeviceWorker::quiesceForRuntimeDowngrade(
    quint64 generation) {
    // DeviceManager invokes this with a blocking queued call after publishing
    // its mutation latch and closing the generation gate. Reaching this slot
    // proves that every earlier worker command returned and that worker-owned
    // transport timers cannot write after the durable marker is committed.
    quiesceDeviceTransports(generation);
}

void DeviceWorker::quiesceForFirmware(const QString &leaseId,
                                      quint64 generation) {
    // This slot is deliberately queued on the same worker thread as every
    // device command. Reaching it proves that all commands accepted before the
    // firmware gate have returned. The generation gate is closed synchronously
    // by DeviceManager before this slot is queued, so in-flight printer-class
    // transactions are interrupted and no later transaction can start.
    quiesceDeviceTransports(generation);

    emit firmwareTransportQuiesced(leaseId, generation);
}

void DeviceWorker::releaseFirmwareQuiesceFence(
    const QString &leaseId, quint64 generation) {
    // This no-op fence shares the device worker queue with quiesce and every
    // transport command. Its ACK proves that a previously queued quiesce can
    // no longer run after DeviceManager resumes the transport.
    emit firmwareQuiesceReleaseFenceReached(
        leaseId, generation);
}

void DeviceWorker::readPrinterDeviceInfo(const QString &devicePath,
                                         quint64 generation) {
    printerSession_->readPrinterDeviceInfo(devicePath, generation);
}

void DeviceWorker::readPrinterDisplayState(const QString &devicePath,
                                           quint64 generation) {
    printerSession_->readPrinterDisplayState(devicePath, generation);
}

void DeviceWorker::refreshPrinterMediaList(const QString &devicePath,
                                           const QString &operationId,
                                           quint64 generation) {
    printerSession_->refreshPrinterMediaList(devicePath, operationId, generation);
}

void DeviceWorker::stagePrinterMedia(
    const QString &devicePath, const QString &mediaName,
    qint64 expectedSize, const QString &outputPath,
    const QString &operationId, quint64 generation) {
    printerSession_->stagePrinterMedia(devicePath, mediaName, expectedSize, outputPath, operationId, generation);
}

void DeviceWorker::preflightReplacePrinterMedia(
    const QString &devicePath, const QString &mediaName,
    qint64 expectedSize,
    const QString &expectedReplacementName,
    qint64 expectedReplacementSize,
    const QString &operationId,
    quint64 generation) {
    printerSession_->preflightReplacePrinterMedia(devicePath, mediaName, expectedSize, expectedReplacementName, expectedReplacementSize, operationId, generation);
}

void DeviceWorker::deletePrinterMedia(
    const QString &devicePath, const QStringList &fileNames,
    const QString &operationId, const QString &deleteIntentPath,
    bool reconcileOnly, qint64 expectedSingleSize,
    const QString &expectedReplacementName,
    qint64 expectedReplacementSize,
    quint64 generation) {
    printerSession_->deletePrinterMedia(devicePath, fileNames, operationId, deleteIntentPath, reconcileOnly, expectedSingleSize, expectedReplacementName, expectedReplacementSize, generation);
}

void DeviceWorker::applyPrinterMedia(const QString &devicePath,
                                     const QString &mediaFile,
                                     const TryxRuntimeApplyRequest &request,
                                     bool updateMetrics,
                                     const QString &proofDeviceIdentity,
                                     const QList<TryxRuntimeSavedMediaRefV1> &proof,
                                     const QString &operationId,
                                     quint64 generation) {
    printerSession_->applyPrinterMedia(devicePath, mediaFile, request, updateMetrics, proofDeviceIdentity, proof, operationId, generation);
}

void DeviceWorker::applyPrinterMediaWithBadgesV1(
    const QString &devicePath, const QString &mediaFile,
    const TryxRuntimeApplyWithBadgesV1 &request, bool updateMetrics,
    const QString &proofDeviceIdentity, const QList<TryxRuntimeSavedMediaRefV1> &proof,
    const QString &operationId, quint64 generation) {
    if (request.schemaVersion != 1) {
        emit printerApplyFinished(operationId, mediaFile, false, updateMetrics,
            PrinterProtocol::MutationOutcome::Rejected,
            QStringLiteral("Unsupported badge Apply format."), generation);
        return;
    }
    printerSession_->applyPrinterMedia(devicePath, mediaFile, request.request, updateMetrics,
        proofDeviceIdentity, proof, operationId, generation, request.badges);
}

void DeviceWorker::configurePrinterMetrics(
    const QString &devicePath,
    const TryxRuntimeMetricsConfigRequest &request,
    const QString &operationId, quint64 generation) {
    printerSession_->configurePrinterMetrics(devicePath, request, operationId, generation);
}

void DeviceWorker::sendPrinterSysinfo(
    const QString &devicePath, const QStringList &labels,
    const QStringList &values, const QStringList &units,
    quint64 generation) {
    printerSession_->sendPrinterSysinfo(devicePath, labels, values, units, generation);
}

void DeviceWorker::sendPrinterMetrics() {
    printerSession_->sendPrinterMetrics();
}

void DeviceWorker::startPrinterDisplaySession(const QString &devicePath,
                                              quint64 generation) {
    printerSession_->startPrinterDisplaySession(devicePath, generation);
}

void DeviceWorker::retryPrinterSessionStart() {
    printerSession_->retryPrinterSessionStart();
}

void DeviceWorker::sendPrinterKeepalive() {
    printerSession_->sendPrinterKeepalive();
}

void DeviceWorker::sendKeepalive() {
    legacySession_->sendKeepalive();
}

void DeviceWorker::setRotation(int degrees) {
    legacySession_->setRotation(degrees);
}

void DeviceWorker::rebootDevice() {
    legacySession_->rebootDevice();
}

quint64 DeviceWorkerSessionContext::generation() const {
    return worker_.printerGenerationGate_.load(std::memory_order_acquire);
}

bool DeviceWorkerSessionContext::generationIsCurrent(quint64 generation) const {
    return worker_.printerEndpointReady_.load(std::memory_order_acquire) &&
           worker_.printerGenerationGate_.load(std::memory_order_acquire) == generation;
}

bool DeviceWorkerSessionContext::operationIsCancelled(
    const QString &operationId) const {
    if (operationId.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&worker_.printerOperationCancellationMutex_);
    return worker_.cancelledPrinterOperationIds_.contains(operationId);
}

int DeviceWorkerSessionContext::generationCancellationFd() const {
    return worker_.printerCancellationFd_;
}

int DeviceWorkerSessionContext::operationCancellationFd() const {
    return worker_.printerOperationCancellationFd_;
}

unsigned int DeviceWorkerSessionContext::publishedPresentationPreferences() const {
    return worker_.publishedPresentationPreferences_.load(std::memory_order_acquire);
}
