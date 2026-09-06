#include "printerprotocol.h"
#include "paseconfigurationclient.h"
#include "pasemediaclient.h"
#include "turrismediaclient.h"
#include "printertransactionchannel.h"
#include "printermediahelpers_p.h"
#include "printerprotocolconstants_p.h"
#include "printerdiscovery_p.h"
#include "usbprintertransport.h"

#include <memory>

using namespace tryx::printer_discovery;
using namespace tryx::printer_protocol_constants;
using namespace tryx::printer_media;

PrinterProtocol::PrinterProtocol()
    : PrinterProtocol(*printerProductProfileForId(kPaseProductId)) {}

PrinterProtocol::PrinterProtocol(int transactionTimeoutMs)
    : PrinterProtocol(*printerProductProfileForId(kPaseProductId),
                      transactionTimeoutMs) {}

PrinterProtocol::PrinterProtocol(int transactionTimeoutMs, int deviceInfoReadyTimeoutMs)
    : PrinterProtocol(*printerProductProfileForId(kPaseProductId), transactionTimeoutMs,
                      deviceInfoReadyTimeoutMs) {}

PrinterProtocol::PrinterProtocol(const PrinterProductProfile &productProfile)
    : productProfile_(productProfile),
      channel_(std::make_unique<PrinterTransactionChannel>(
          productProfile_.productId, 3000, kFileTransmitResponseTimeoutMs)),
      configuration_(std::make_unique<PaseConfigurationClient>(
          *channel_, productProfile_, kDeviceInformationReadinessDeadlineMs)),
      media_(std::make_unique<PaseMediaClient>(*channel_, productProfile_)),
      turris_(std::make_unique<TurrisMediaClient>(*channel_, productProfile_)) {}

PrinterProtocol::PrinterProtocol(const PrinterProductProfile &productProfile,
                                 int transactionTimeoutMs)
    : productProfile_(productProfile),
      channel_(std::make_unique<PrinterTransactionChannel>(
          productProfile_.productId, transactionTimeoutMs, transactionTimeoutMs)),
      configuration_(std::make_unique<PaseConfigurationClient>(
          *channel_, productProfile_, transactionTimeoutMs)),
      media_(std::make_unique<PaseMediaClient>(*channel_, productProfile_)),
      turris_(std::make_unique<TurrisMediaClient>(*channel_, productProfile_)) {}

PrinterProtocol::PrinterProtocol(const PrinterProductProfile &productProfile,
                                 int transactionTimeoutMs, int deviceInfoReadyTimeoutMs)
    : productProfile_(productProfile),
      channel_(std::make_unique<PrinterTransactionChannel>(
          productProfile_.productId, transactionTimeoutMs, transactionTimeoutMs)),
      configuration_(std::make_unique<PaseConfigurationClient>(
          *channel_, productProfile_, deviceInfoReadyTimeoutMs)),
      media_(std::make_unique<PaseMediaClient>(*channel_, productProfile_)),
      turris_(std::make_unique<TurrisMediaClient>(*channel_, productProfile_)) {}

PrinterProtocol::~PrinterProtocol() = default;

bool PrinterProtocol::isSafeUploadMediaName(const QString &fileName) {
    return isSafeUploadFileName(fileName);
}

void PrinterProtocol::close() { channel_->closeDevice(); }

const PrinterProductProfile &PrinterProtocol::productProfile() const {
    return productProfile_;
}

bool PrinterProtocol::persistentUsbInputFailure() const {
    return channel_->persistentUsbInputFailure();
}

PrinterProtocol::Result
PrinterProtocol::startDisplaySession(const QString &devicePath,
                                     const OperationContext &context) {
    if (productProfile_.idleMode == PrinterIdleMode::TransferOnly) {
        return turris_->startDisplaySession(devicePath, context);
    }
    return configuration_->startDisplaySession(devicePath, context);
}

PrinterProtocol::Result
PrinterProtocol::readDeviceInfo(const QString &devicePath,
                                const OperationContext &context) {
    if (productProfile_.idleMode == PrinterIdleMode::TransferOnly) {
        return turris_->readDeviceInfo(devicePath, context);
    }
    return configuration_->readDeviceInfo(devicePath, context);
}

PrinterProtocol::MediaListResult
PrinterProtocol::readMediaList(const QString &devicePath,
                               const OperationContext &context) {
    return media_->readMediaList(devicePath, context);
}

PrinterProtocol::MediaPullResult
PrinterProtocol::pullUserMedia(const QString &devicePath, const QString &mediaName,
                               qint64 expectedSize, const MediaPullChunkSink &sink,
                               const MediaPullProgress &progress,
                               const OperationContext &context) {
    return media_->pullUserMedia(devicePath, mediaName, expectedSize, sink, progress,
                                 context);
}

PrinterProtocol::MediaReferenceResult PrinterProtocol::readUserMediaReferences(
    const QString &devicePath, const QString &mediaName, qint64 expectedSize,
    const QString &expectedReplacementName, qint64 expectedReplacementSize,
    const OperationContext &context) {
    return media_->readUserMediaReferences(devicePath, mediaName, expectedSize,
                                           expectedReplacementName,
                                           expectedReplacementSize, context);
}

PrinterProtocol::DeleteResult PrinterProtocol::removeUserMedia(
    const QString &devicePath, const QStringList &fileNames,
    const BeforeDeleteDispatch &beforeDispatch, const DeleteProgress &progress,
    const OperationContext &context, bool reconcileOnly, qint64 expectedSingleSize,
    const QString &expectedReplacementName, qint64 expectedReplacementSize) {
    return media_->removeUserMedia(devicePath, fileNames, beforeDispatch, progress,
                                   context, reconcileOnly, expectedSingleSize,
                                   expectedReplacementName, expectedReplacementSize);
}

bool PrinterProtocol::uploadMedia(const QString &devicePath, const QString &localPath,
                                  const QString &remoteFileName, QString *uploadedName,
                                  QString *errorMessage, const UploadProgress &progress,
                                  const OperationContext &context,
                                  MutationDetails *mutationDetails,
                                  const QString &expectedSha256) {
    if (productProfile_.productId == kTurrisProductId) {
        return turris_->uploadMedia(devicePath, localPath, remoteFileName, uploadedName,
                                    errorMessage, progress, context, mutationDetails,
                                    expectedSha256);
    }
    return media_->uploadMedia(devicePath, localPath, remoteFileName, uploadedName,
                               errorMessage, progress, context, mutationDetails,
                               expectedSha256);
}

bool PrinterProtocol::applyPresetMedia(const QString &devicePath,
                                       const QString &mediaFile, int brightness,
                                       QString *errorMessage,
                                       const OperationContext &context,
                                       MutationDetails *mutationDetails) {
    return configuration_->applyPresetMedia(devicePath, mediaFile, brightness,
                                            errorMessage, context, mutationDetails);
}

bool PrinterProtocol::applyPresetMediaWithOverlay(
    const QString &devicePath, const QString &mediaFile, int brightness,
    const PaseOverlayConfig &overlay, QString *errorMessage,
    const OperationContext &context, MutationDetails *mutationDetails) {
    return configuration_->applyPresetMediaWithOverlay(
        devicePath, mediaFile, brightness, overlay, errorMessage, context,
        mutationDetails);
}

PrinterProtocol::PaseDisplayStateResult
PrinterProtocol::readPaseDisplayState(const QString &devicePath,
                                      const OperationContext &context) {
    return configuration_->readPaseDisplayState(devicePath, context);
}

bool PrinterProtocol::applyPaseConfiguration(const QString &devicePath,
                                             const PaseApplyConfig &config,
                                             QString *errorMessage,
                                             const OperationContext &context,
                                             MutationDetails *mutationDetails,
                                             PaseDisplayState *appliedState) {
    return configuration_->applyPaseConfiguration(
        devicePath, config, errorMessage, context, mutationDetails, appliedState);
}

bool PrinterProtocol::configurePaseOverlay(const QString &devicePath,
                                           const PaseOverlayConfig &overlay,
                                           QString *errorMessage,
                                           const OperationContext &context,
                                           MutationDetails *mutationDetails) {
    return configuration_->configurePaseOverlay(devicePath, overlay, errorMessage,
                                                context, mutationDetails);
}

bool PrinterProtocol::sendPaseMetricBatch(
    const QString &devicePath, const PaseOverlayConfig &overlay,
    const QStringList &labels, const QStringList &values, const QStringList &units,
    QString *errorMessage, const OperationContext &context) {
    return configuration_->sendPaseMetricBatch(devicePath, overlay, labels, values,
                                               units, errorMessage, context);
}

bool PrinterProtocol::setBrightness(const QString &devicePath, int brightness,
                                    QString *errorMessage,
                                    const OperationContext &context) {
    return configuration_->setBrightness(devicePath, brightness, errorMessage, context);
}

PrinterProtocol::KeepaliveOutcome
PrinterProtocol::sendKeepalive(const QString &devicePath, QString *errorMessage,
                               const OperationContext &context) {
    return configuration_->sendKeepalive(devicePath, errorMessage, context);
}

PrinterProtocol::KeepaliveOutcome
PrinterProtocol::sendDisplayKeepalive(const QString &devicePath, QString *errorMessage,
                                      const OperationContext &context,
                                      const PaseOverlayConfig *overlay) {
    return configuration_->sendDisplayKeepalive(devicePath, errorMessage, context,
                                                overlay);
}

int PrinterProtocol::millisecondsUntilKeepalive() const {
    return channel_->millisecondsUntilKeepalive();
}

#ifdef TRYX_PROTOCOL_TESTING
bool PrinterProtocol::trackedPingForTesting(const QString &devicePath, QString *payload,
                                            QString *errorMessage,
                                            const OperationContext &context) {
    panorama::wire::v1::Request request;
    request.mutable_ping()->set_payload("hello?");
    panorama::wire::v1::Response response;
    if (!channel_->execute(&request, panorama::wire::v1::Response::kPong, &response,
                           devicePath, context, errorMessage)) {
        return false;
    }
    if (payload) {
        *payload = QString::fromStdString(response.pong().payload());
    }
    return true;
}

void PrinterProtocol::adoptFileDescriptorForTesting(int fd, const QString &devicePath) {
    channel_->adoptFileDescriptor(fd, devicePath);
}

void PrinterProtocol::setUnframedRecoveryEligibleForTesting(bool eligible) {
    channel_->setUnframedRecoveryEligibleForTesting(eligible);
}

void PrinterProtocol::setFileTransmitDataWriteTimeoutForTesting(int timeoutMs) {
    channel_->setFileTransmitDataWriteTimeoutForTesting(timeoutMs);
}

void PrinterProtocol::setFileTransmitResponseTimeoutForTesting(int timeoutMs) {
    channel_->setFileTransmitResponseTimeoutForTesting(timeoutMs);
}

void PrinterProtocol::setMediaPullLimitsForTesting(qint64 maximumBytes,
                                                   int maximumChunks, int deadlineMs) {
    media_->setMediaPullLimitsForTesting(maximumBytes, maximumChunks, deadlineMs);
}

void PrinterProtocol::setPersistentUsbInputFailureForTesting(bool persistent) {
    channel_->setPersistentUsbInputFailureForTesting(persistent);
}

void PrinterProtocol::setBootstrapZeroByteWriteFailuresForTesting(int failureCount) {
    configuration_->setBootstrapZeroByteWriteFailuresForTesting(failureCount);
}

QList<qint64> PrinterProtocol::bootstrapReadinessAttemptOffsetsForTesting() const {
    return configuration_->bootstrapReadinessAttemptOffsetsForTesting();
}

bool PrinterProtocol::sendPaseRunConfigForTesting(const QString &devicePath,
                                                  const PaseOverlayConfig &overlay,
                                                  QString *errorMessage,
                                                  const OperationContext &context) {
    return configuration_->sendRunConfigTrigger(devicePath, errorMessage, context,
                                                &overlay);
}

bool PrinterProtocol::validateEndpointForTesting(const QString &devicePath, int openFd,
                                                 const QString &sysfsRoot,
                                                 const QString &devRoot,
                                                 QString *errorMessage) {
    return validatePrinterEndpoint(devicePath, openFd, sysfsRoot, devRoot,
                                   kPaseProductId, errorMessage);
}

bool PrinterProtocol::validateEndpointForTesting(const QString &devicePath, int openFd,
                                                 const QString &sysfsRoot,
                                                 const QString &devRoot,
                                                 quint16 expectedProductId,
                                                 QString *errorMessage) {
    return validatePrinterEndpoint(devicePath, openFd, sysfsRoot, devRoot,
                                   expectedProductId, errorMessage);
}

QByteArray PrinterProtocol::applyMediaPullXorForTesting(const QByteArray &bytes,
                                                        quint64 absoluteOffset) {
    return applyMediaPullXor(bytes, absoluteOffset, nullptr, nullptr);
}

bool PrinterProtocol::validateMediaPullPathForTesting(const QByteArray &rawPath,
                                                      const QString &mediaName) {
    return validateMediaPullPath(rawPath, mediaName);
}

PrinterProtocol::DuplexTestResult PrinterProtocol::runDuplexTransportScenarioForTesting(
    const QList<DuplexTestEvent> &events, const QByteArray &request, int writeTimeoutMs,
    int readTimeoutMs) {
    return UsbPrinterTransport::runScenarioForTesting(events, request, writeTimeoutMs,
                                                      readTimeoutMs);
}
#endif
