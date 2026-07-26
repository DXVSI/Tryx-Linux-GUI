#include <QtTest>
#include <QtDBus>

#include "printerprotocol.h"
#include "devicemanager.h"
#include "runtimebridge.h"
#include "systemmonitor.h"
#include "panoramapage.h"
#include "displaypage.h"
#include "splitconfig.h"

#include "overlay.pb.h"
#include "transport.pb.h"
#include "configuration.pb.h"

#include <QCryptographicHash>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/unknown_field_set.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kPeerTimeoutMs = 2000;

bool writeAllFd(int fd, const QByteArray &data, QString *errorMessage) {
    qsizetype offset = 0;
    QElapsedTimer timer;
    timer.start();
    while (offset < data.size() && timer.elapsed() < kPeerTimeoutMs) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLOUT;
        const int remaining = kPeerTimeoutMs - static_cast<int>(timer.elapsed());
        const int pollResult = ::poll(&descriptor, 1, qMax(1, remaining));
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult <= 0 ||
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("peer write poll failed");
            }
            return false;
        }
        const ssize_t written = ::write(fd, data.constData() + offset,
                                        static_cast<size_t>(data.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("peer write failed: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
            }
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    if (offset != data.size() && errorMessage) {
        *errorMessage = QStringLiteral("peer write timed out");
    }
    return offset == data.size();
}

bool readFrameFd(int fd, QByteArray *payload, QString *errorMessage,
                 int timeoutMs = kPeerTimeoutMs,
                 QByteArray *persistentBuffer = nullptr) {
    QByteArray localBuffer;
    QByteArray *buffer = persistentBuffer ? persistentBuffer : &localBuffer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const auto status = PrinterFrameCodec::takeFrame(buffer, payload, errorMessage);
        if (status == PrinterFrameCodec::DecodeStatus::FrameReady) {
            return true;
        }
        if (status == PrinterFrameCodec::DecodeStatus::Malformed) {
            return false;
        }

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        const int pollResult = ::poll(&descriptor, 1, qMax(1, remaining));
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult <= 0 ||
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("peer read poll failed");
            }
            return false;
        }

        char chunk[65536];
        const ssize_t size = ::read(fd, chunk, sizeof(chunk));
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size <= 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("peer read failed");
            }
            return false;
        }
        buffer->append(chunk, static_cast<qsizetype>(size));
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("peer read timed out");
    }
    return false;
}

bool readRequest(int fd, panorama::wire::v1::Request *request,
                 QString *errorMessage, int timeoutMs = kPeerTimeoutMs,
                 QByteArray *persistentBuffer = nullptr) {
    QByteArray payload;
    return readFrameFd(fd, &payload, errorMessage, timeoutMs,
                       persistentBuffer) &&
           request &&
           request->ParseFromArray(payload.constData(), static_cast<int>(payload.size()));
}

bool writeResponse(int fd, const panorama::wire::v1::Response &response,
                   QString *errorMessage) {
    std::string serialized;
    if (!response.SerializeToString(&serialized)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("response serialization failed");
        }
        return false;
    }
    return writeAllFd(fd, PrinterFrameCodec::encode(QByteArray(
                              serialized.data(), static_cast<qsizetype>(serialized.size()))),
                      errorMessage);
}

bool waitForPeerClosureWithoutPayload(
    int fd, int timeoutMs, QString *errorMessage) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int remaining =
            timeoutMs - static_cast<int>(timer.elapsed());
        const int pollResult =
            ::poll(&descriptor, 1, qMax(1, remaining));
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "peer closure poll failed");
            }
            return false;
        }
        if (pollResult == 0) {
            break;
        }

        char byte = 0;
        const ssize_t received =
            ::recv(
                fd, &byte, sizeof(byte),
                MSG_DONTWAIT | MSG_PEEK);
        if (received > 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "unexpected request before peer transport closure");
            }
            return false;
        }
        if (received == 0 ||
            (descriptor.revents &
             (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return true;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "peer closure read failed");
            }
            return false;
        }
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "peer transport remained open");
    }
    return false;
}

bool writeUnframedResponse(
    int fd, const panorama::wire::v1::Response &response,
    QString *errorMessage) {
    std::string serialized;
    if (!response.SerializeToString(&serialized)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "unframed response serialization failed");
        }
        return false;
    }
    return writeAllFd(
        fd,
        QByteArray(serialized.data(),
                   static_cast<qsizetype>(serialized.size())),
        errorMessage);
}

panorama::wire::v1::Response baseResponse(
    const panorama::wire::v1::Request &request, qint64 trackOffset = 0) {
    panorama::wire::v1::Response response;
    response.mutable_header()->set_version(1);
    response.mutable_header()->set_track_id(
        static_cast<quint64>(static_cast<qint64>(request.header().track_id()) + trackOffset));
    response.mutable_header()->set_payload_crc32(0);
    return response;
}

bool serveUdbBootstrap(int fd, QString *errorMessage,
                       QByteArray *persistentBuffer = nullptr) {
    const QList<panorama::wire::v1::Request::BodyCase> expectedBodies = {
        panorama::wire::v1::Request::kDeviceInformationQuery,
        panorama::wire::v1::Request::kSystemConfigurationQuery,
        panorama::wire::v1::Request::kDeviceAuthenticationQuery
    };
    const QList<QByteArray> expectedFrames = {
        QByteArray::fromHex("545259580b0000000a020801a206040a024e41"),
        QByteArray::fromHex("545259580b0000000a020801b206040a024e41"),
        QByteArray::fromHex("54525958090000000a020801aa06020801")
    };
    QByteArray localBuffer;
    QByteArray *receiveBuffer =
        persistentBuffer ? persistentBuffer : &localBuffer;
    for (int index = 0; index < expectedBodies.size(); ++index) {
        QByteArray payload;
        if (!readFrameFd(fd, &payload, errorMessage, kPeerTimeoutMs,
                         receiveBuffer)) {
            return false;
        }
        panorama::wire::v1::Request request;
        if (!request.ParseFromArray(payload.constData(),
                                    static_cast<int>(payload.size()))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "failed to parse UDB bootstrap request %1").arg(index);
            }
            return false;
        }
        if (request.body_case() != expectedBodies.at(index) ||
            !request.has_header() || request.header().version() != 1 ||
            request.header().track_id() != 0 ||
            request.header().payload_crc32() != 0 ||
            request.header().ByteSizeLong() != 2 ||
            PrinterFrameCodec::encode(payload) != expectedFrames.at(index)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "unexpected UDB bootstrap request %1").arg(index);
            }
            return false;
        }
        if (index < expectedBodies.size() - 1) {
            pollfd descriptor{};
            descriptor.fd = fd;
            descriptor.events = POLLIN;
            const int pollResult = receiveBuffer->isEmpty()
                ? ::poll(&descriptor, 1, 50)
                : 1;
            if (pollResult < 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                        "failed to inspect bootstrap request ordering");
                }
                return false;
            }
            if (pollResult > 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                        "next bootstrap request arrived before the previous response");
                }
                return false;
            }
        }

        auto response = baseResponse(request);
        if (index == 0) {
            auto *deviceInfo = response.mutable_device_information();
            deviceInfo->set_product_name("PANORAMA SE");
            deviceInfo->set_firmware_version("test-firmware");
            deviceInfo->set_serial_number("test-serial");
        } else if (index == 1) {
            response.mutable_system_configuration();
        } else {
            response.mutable_device_authentication()->set_auth("test-auth");
        }
        if (!writeResponse(fd, response, errorMessage)) {
            return false;
        }
    }
    return true;
}

bool createSocketPair(int sockets[2], QString *errorMessage) {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QString::fromLocal8Bit(std::strerror(errno));
    }
    return false;
}

bool writeTextFile(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

bool createUsbDevice(const QString &sysRoot, const QString &name,
                     const QByteArray &productId) {
    const QString devicePath = QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/") + name);
    return writeTextFile(QDir(devicePath).filePath(QStringLiteral("idVendor")), "391a\n") &&
           writeTextFile(QDir(devicePath).filePath(QStringLiteral("idProduct")),
                         productId + '\n') &&
           writeTextFile(QDir(devicePath).filePath(QStringLiteral("manufacturer")), "TRYX\n") &&
           writeTextFile(QDir(devicePath).filePath(QStringLiteral("product")), "RK PASE\n") &&
           writeTextFile(QDir(devicePath).filePath(QStringLiteral("serial")),
                         name.toUtf8() + '\n');
}

bool createPrinterEndpoint(const QString &sysRoot, const QString &devRoot,
                           const QString &usbName, const QString &lpName) {
    const QString usbDevicePath =
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/") + usbName);
    const QString interfacePath = QDir(usbDevicePath).filePath(usbName + QStringLiteral(":1.0"));
    if (!writeTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceClass")), "07\n") ||
        !writeTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceSubClass")), "01\n") ||
        !writeTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceProtocol")), "02\n")) {
        return false;
    }

    const QString classPath =
        QDir(sysRoot).filePath(QStringLiteral("class/usbmisc/") + lpName);
    QDir().mkpath(classPath);
    const QString linkPath = QDir(classPath).filePath(QStringLiteral("device"));
    const QByteArray encodedTarget = QFile::encodeName(interfacePath);
    const QByteArray encodedLink = QFile::encodeName(linkPath);
    if (::symlink(encodedTarget.constData(), encodedLink.constData()) != 0) {
        return false;
    }

    const QString endpointPath = QDir(devRoot).filePath(QStringLiteral("usb/") + lpName);
    return writeTextFile(endpointPath, {});
}

bool hasUnknownField(const google::protobuf::UnknownFieldSet &fields, int number) {
    for (int index = 0; index < fields.field_count(); ++index) {
        if (fields.field(index).number() == number) {
            return true;
        }
    }
    return false;
}

}  // namespace

class RuntimeRoundTripObject final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Test")

public slots:
    TryxRuntimeOperationsSnapshot Echo(
        const TryxRuntimeOperationsSnapshot &snapshot) const {
        return snapshot;
    }
    TryxRuntimeMediaCatalogSnapshot EchoMediaCatalog(
        const TryxRuntimeMediaCatalogSnapshot &snapshot) const {
        return snapshot;
    }
    TryxRuntimeLegacyMediaCatalogSnapshot EchoLegacyMediaCatalog(
        const TryxRuntimeLegacyMediaCatalogSnapshot &snapshot) const {
        return snapshot;
    }
    TryxRuntimeMetricsConfigRequest EchoMetricsRequest(
        const TryxRuntimeMetricsConfigRequest &request) const {
        return request;
    }
    TryxRuntimeMetricsState EchoMetricsState(
        const TryxRuntimeMetricsState &state) const {
        return state;
    }
    TryxRuntimeApplyRequest EchoApplyRequest(
        const TryxRuntimeApplyRequest &request) const {
        return request;
    }
    TryxRuntimeDisplayState EchoDisplayState(
        const TryxRuntimeDisplayState &state) const {
        return state;
    }
};

class PrinterProtocolTests : public QObject {
    Q_OBJECT

private slots:
    void fragmentedFrame();
    void concatenatedFrames();
    void malformedAndOversizedFrames();
    void runtimeOperationDbusRoundTrip();
    void runtimeMediaCatalogDbusRoundTrip();
    void runtimeLegacyMediaCatalogDbusRoundTrip();
    void runtimeMetricsDbusRoundTrip();
    void runtimeDisplayConfigDbusRoundTrip();
    void remoteDisplayStateRequiresStrictlyIncreasingRevision();
    void panoramaPageRestoresDisplayAndSplitState();
    void panoramaPageOmitsPresetSurface();
    void panoramaBrightnessCoalescesUntilTransportReady();
    void paseRunConfigUsesWireLayout();
    void paseWaterfallFullScreenGeometry_data();
    void paseWaterfallFullScreenGeometry();
    void paseWaterfallSplitGeometryAndIndependentStyles();
    void paseSplitApplyBuildsDualUserConfigAndBadges();
    void paseApplyRetriesDroppedReadOnlyConfigResponse_data();
    void paseApplyRetriesDroppedReadOnlyConfigResponse();
    void paseApplyReadOnlyRetryCancellationSendsNoMutation();
    void paseApplyReadOnlyRetryBudgetIsBounded();
    void paseApplyMismatchedReadOnlyResponseDoesNotRetry();
    void paseReadbackMismatchIsVerificationFailure();
    void runConfigRejectionIsVerificationFailureAndKeepsTransport();
    void verificationFailureKeepsHealthySessionActive();
    void applyFailureResultPrecedesSessionLoss();
    void lateRunConfigDummyDoesNotBreakReadback();
    void paseDisplayMutationMatrix_data();
    void paseDisplayMutationMatrix();
    void readPaseDisplayStateDecodesDualConfiguration();
    void standalonePaseMetricsConfigurationIsAcknowledged();
    void displayKeepalivePreservesPaseOverlayValues();
    void paseMetricBatchMatchesHeaderlessWireFrame();
    void metricBatchExplicitErrorIsRejected();
    void metricBatchResponsesAreDrainedBeforeTrackedRequest();
    void daemonMetricsBatchRunsWithoutGui();
    void foregroundOperationPausesMetricsAndKeepalive();
    void raplPowerUsesMonotonicBoundedInterval();
    void hardwareBadgeModelsResolve();
    void metricsConfigurationValidatesPersistsAndDisables();
    void paseOverlayV1MigrationLoadsSingleArea();
    void paseSchedulerAcceptsSplitAndDisplayOnly();
    void mediaCatalogPersistsThumbnailAndPrunesAuthoritatively();
    void mediaCatalogV1MigrationStripsForgedOrigin();
    void mediaCatalogV1MigrationRejectsStaleRollback();
    void ensureMediaReusesOriginWithoutPreparation();
    void ensureOriginMissReleasesForegroundBeforePreparation();
    void previewCommitFailureRemainsRetryable_data();
    void previewCommitFailureRemainsRetryable();
    void successfulUploadDoesNotDeleteUnrelatedRetryCandidate();
    void schedulerRejectsConcurrentOperationAndKeepsExactIdentity();
    void uploadWithoutDeviceIdentityIsRejected();
    void queuedApplyCancellationIsNotDrainedOrExecuted();
    void retryPreflightAlwaysUsesFreshNameAfterPartialUpload();
    void startupRetryCacheValidationSerializesMutations();
    void retryV9PreservesApplyRequest();
    void ensureRetryManifestRequiresOriginIdentity();
    void retryCacheHashValidationRejectsMismatch();
    void retryCacheHashValidationHonorsPreStartCancellation();
    void retryCancellationRetainsOwnershipOnManifestRemovalFailure();
    void mediaPreparationBoundsAndThumbnailFallback();
    void shutdownPreservesPreparedMediaAsUnknownRetry();
    void postUploadRefreshFailureRemainsUnknownAndRetryable();
    void finalizationUnknownAutoReconcilesWithoutRetransmission();
    void finalizationUnknownRebindsAfterUsbGenerationChange();
    void finalizationUnknownDoesNotAdoptReplacementDevice();
    void finalizationUnknownRestartAutoReconcilesWithoutRetry();
    void finalizationReconciliationMissSurvivesRestartAsFreshNameRetry();
    void applyPreflightTimeoutPreservesNotStartedBeforeSessionLoss();
    void generationChangeWaitsForStructuredApplyOutcome();
    void wireCriticalGoldenFixtures();
    void unknownFieldsSurviveMutation();
    void discoveryStateSequence();
    void productionEndpointValidationWithOfflineSysfs();
    void paseUdevReadinessUsesUsbDeviceEvents();
    void samePathEndpointEventForcesNewEpochSignal();
    void unrelatedUsbRemoveDoesNotRestartPaseSession();
    void passivePrinterReconnectDoesNotRequestSessionResume();
    void samePathReenumerationCancelsOldGeneration();
    void printerSessionLossCancelsOperationsAndReportsStoppedState();
    void lostPrinterSessionRejectsMutationsBeforeDispatch();
    void lostPrinterSessionRequiresObservedRemovalBeforeReconnect();
    void sessionNotReadyRejectsMutationsBeforeDispatch();
    void persistentUsbInputFailureStopsSameGenerationWithoutRecovery();
    void partialUploadRequiresObservedDeviceRemovalBeforeRetry();
    void tamperedV8RetryManifestIsRejected_data();
    void tamperedV8RetryManifestIsRejected();
    void legacyRetryManifestRestoresPartialTransferHazard();
    void legacyUnknownOutcomeRequiresObservedRecovery();
    void recoveredV6ManifestMigratesToFreshNameRetry();
    void monitoringFailureIsFailClosed();
    void typedPingTransaction();
    void unframedTrackedResponseAfterTransportError();
    void unframedTrackedResponseRequiresTransportError();
    void unframedWrongTrackOrBodyDoesNotBypassMatching();
    void udbSessionBootstrapUsesExactSequence();
    void udbSessionBootstrapWaitsForLateDeviceInfo();
    void udbSessionBootstrapResynchronizesAfterStaleTail();
    void udbSessionBootstrapSkipsStaleTrackedResponse();
    void udbSessionBootstrapRejectsUnboundedStaleResponses();
    void udbSessionDeviceInfoReadinessTimeoutSendsOnce();
    void udbSessionBootstrapPartialResponseIsTerminal();
    void udbSessionDeviceInfoRetriesConfirmedZeroByteOutWithBackoff();
    void udbSessionDeviceInfoCancellationDuringBackoffIsTerminal();
    void udbSessionBootstrapSendsPostReadinessExchangesOnce();
    void udbKeepaliveUsesExactUntrackedFrame();
    void udbKeepaliveDrainsOptionalPong();
    void udbKeepaliveZeroByteWriteFailureIsRetryable();
    void displayKeepaliveIsWriteDrivenAndDrainsOptionalAck();
    void displayKeepaliveTransportFailureIsRetryable();
    void displayKeepaliveMalformedResponseIsDiscarded();
    void displayKeepaliveIncompleteResponseIsDiscarded();
    void notificationBeforeExpectedResponse();
    void headerlessPongBeforeTrackedResponseIsSkipped();
    void boundedResponseBurstBeforeFileListIsSkipped();
    void slowTrackedResponseGetsInFlightKeepalive();
    void unrelatedTrackIsSkipped();
    void timeoutIsBounded();
    void cancellationFdStopsTransaction();
    void cancellationFdInterruptsBlockedResponse();
    void userCancellationDoesNotInterruptSessionRecovery();
    void typedFileListTransaction();
    void deleteUserMediaLostAckReconcilesWithoutReplay();
    void deleteUserMediaAcceptsHeaderOnlySuccess();
    void deleteUserMediaRejectsReferencedFileBeforeDispatch();
    void deleteUserMediaRetainsUnknownAfterBoundedReconciliation();
    void deleteReconcileOnlyNeverDispatchesFileRemove();
    void deleteIntentSurvivesRestartAndOnlyReconcilesSameDevice();
    void passivePrinterWorkerSendsNoFrames();
    void printerRefreshStartsSessionAndKeepalive();
    void runConfigErrorResponseRejectsSessionStart();
    void restoredOverlayWaitsForKeepaliveBeforeSessionReady();
    void restoredOverlayFailureBecomesLostWithoutReplay();
    void overlayLeaseRejectionBecomesLostWithoutRecovery();
    void applyMediaPreservesUnknownFields();
    void rejectedApplyDoesNotSendRunConfig();
    void userConfigOutcomeUnknownAfterFullSendIsPartial_data();
    void userConfigOutcomeUnknownAfterFullSendIsPartial();
    void runConfigFailureAfterAcceptedUserConfigIsPartial_data();
    void runConfigFailureAfterAcceptedUserConfigIsPartial();
    void incompleteUserConfigIsNotWritten_data();
    void incompleteUserConfigIsNotWritten();
    void paseStandbyMutationIsRejectedBeforeUsb();
    void unsafeMediaNamesAreRejected();
    void duplexInputReceivesAckDuringOutput();
    void duplexZeroLengthInputDefersRearmUntilOutputCompletes();
    void duplexInputErrorDefersRearmWithoutStarvingOutput();
    void duplexInputRetryBudgetIsBounded();
    void uploadIsResponseDriven();
    void uploadDataUsesDedicatedWriteDeadline();
    void uploadWaitsForDelayedBoundaryAckWithIdleKeepalive();
    void uploadEndTimeoutIsFinalizationUnknown();
    void uploadFailureClosesSession_data();
    void uploadFailureClosesSession();
    void uploadCancellationStopsBeforeNextChunk();
    void uploadRejectsSymlinkAndHashMismatchBeforeUsb();
    void uploadSourceMutationIsRejected_data();
    void uploadSourceMutationIsRejected();
};

void PrinterProtocolTests::fragmentedFrame() {
    const QByteArray expected("protobuf-payload");
    const QByteArray frame = PrinterFrameCodec::encode(expected);
    QByteArray buffer;
    QByteArray payload;
    for (qsizetype index = 0; index < frame.size() - 1; ++index) {
        buffer.append(frame.at(index));
        QCOMPARE(PrinterFrameCodec::takeFrame(&buffer, &payload),
                 PrinterFrameCodec::DecodeStatus::NeedMoreData);
    }
    buffer.append(frame.back());
    QCOMPARE(PrinterFrameCodec::takeFrame(&buffer, &payload),
             PrinterFrameCodec::DecodeStatus::FrameReady);
    QCOMPARE(payload, expected);
    QVERIFY(buffer.isEmpty());
}

void PrinterProtocolTests::concatenatedFrames() {
    QByteArray buffer = PrinterFrameCodec::encode("first") +
                        PrinterFrameCodec::encode("second");
    QByteArray payload;
    QCOMPARE(PrinterFrameCodec::takeFrame(&buffer, &payload),
             PrinterFrameCodec::DecodeStatus::FrameReady);
    QCOMPARE(payload, QByteArray("first"));
    QCOMPARE(PrinterFrameCodec::takeFrame(&buffer, &payload),
             PrinterFrameCodec::DecodeStatus::FrameReady);
    QCOMPARE(payload, QByteArray("second"));
    QVERIFY(buffer.isEmpty());
}

void PrinterProtocolTests::malformedAndOversizedFrames() {
    QByteArray garbage("NOPE\0\0\0\0", 8);
    QString error;
    QCOMPARE(PrinterFrameCodec::takeFrame(&garbage, nullptr, &error),
             PrinterFrameCodec::DecodeStatus::Malformed);
    QVERIFY(error.contains(QStringLiteral("magic"), Qt::CaseInsensitive));

    const quint32 size = static_cast<quint32>(PrinterFrameCodec::MaxPayloadSize + 1);
    QByteArray oversized("TRYX", 4);
    oversized.append(static_cast<char>(size & 0xff));
    oversized.append(static_cast<char>((size >> 8) & 0xff));
    oversized.append(static_cast<char>((size >> 16) & 0xff));
    oversized.append(static_cast<char>((size >> 24) & 0xff));
    error.clear();
    QCOMPARE(PrinterFrameCodec::takeFrame(&oversized, nullptr, &error),
             PrinterFrameCodec::DecodeStatus::Malformed);
    QVERIFY(error.contains(QStringLiteral("large"), Qt::CaseInsensitive));
}

void PrinterProtocolTests::runtimeOperationDbusRoundTrip() {
    registerTryxRuntimeMetaTypes();

    TryxRuntimeOperationInfo expectedInfo;
    expectedInfo.id = QStringLiteral("11111111-1111-4111-8111-111111111111");
    expectedInfo.parentId = QStringLiteral("22222222-2222-4222-8222-222222222222");
    expectedInfo.kind = QStringLiteral("UploadRetry");
    expectedInfo.state = QStringLiteral("RetryAvailable");
    expectedInfo.stage = QStringLiteral("RefreshingMedia");
    expectedInfo.errorCategory = QStringLiteral("PartialOrUnknown");
    expectedInfo.terminalOutcome = QStringLiteral("PartialOrUnknown");
    expectedInfo.primaryErrorCategory =
        QStringLiteral("TransportTimeout");
    expectedInfo.primaryErrorMessage =
        QStringLiteral("DataStatus body 801 was not received");
    expectedInfo.retryMode = QStringLiteral("PreparedMedia");
    expectedInfo.subject = QStringLiteral("source.png");
    expectedInfo.resultName = QStringLiteral("target.png.h264_2240x1080");
    expectedInfo.message = QStringLiteral("diagnostic");
    expectedInfo.completed = 123;
    expectedInfo.total = 456;
    expectedInfo.confirmedBytes = 123;
    expectedInfo.lastConfirmedChunkIndex = 7;
    expectedInfo.attempt = 3;
    expectedInfo.deviceGeneration = 42;
    expectedInfo.applyAfterUpload = true;

    TryxRuntimeOperationsSnapshot expectedSnapshot;
    expectedSnapshot.revision = 17;
    expectedSnapshot.activeOperationId = expectedInfo.id;
    expectedSnapshot.operations.append(expectedInfo);

    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.isConnected(), qPrintable(bus.lastError().message()));
    RuntimeRoundTripObject serviceObject;
    const QString objectPath = QStringLiteral("/org/tryx/Panorama/Test/%1")
                                   .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus.registerObject(objectPath, &serviceObject,
                                QDBusConnection::ExportAllSlots),
             qPrintable(bus.lastError().message()));
    QDBusInterface interface(bus.baseService(), objectPath,
                             QStringLiteral("org.tryx.Panorama.Test"), bus);
    const QDBusReply<TryxRuntimeOperationsSnapshot> reply = interface.call(
        QStringLiteral("Echo"), QVariant::fromValue(expectedSnapshot));
    bus.unregisterObject(objectPath);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    const TryxRuntimeOperationsSnapshot decoded = reply.value();

    QCOMPARE(decoded.revision, expectedSnapshot.revision);
    QCOMPARE(decoded.activeOperationId, expectedSnapshot.activeOperationId);
    QCOMPARE(decoded.operations.size(), 1);
    const TryxRuntimeOperationInfo actualInfo = decoded.operations.first();
    QCOMPARE(actualInfo.id, expectedInfo.id);
    QCOMPARE(actualInfo.parentId, expectedInfo.parentId);
    QCOMPARE(actualInfo.kind, expectedInfo.kind);
    QCOMPARE(actualInfo.state, expectedInfo.state);
    QCOMPARE(actualInfo.stage, expectedInfo.stage);
    QCOMPARE(actualInfo.errorCategory, expectedInfo.errorCategory);
    QCOMPARE(actualInfo.terminalOutcome, expectedInfo.terminalOutcome);
    QCOMPARE(actualInfo.primaryErrorCategory,
             expectedInfo.primaryErrorCategory);
    QCOMPARE(actualInfo.primaryErrorMessage,
             expectedInfo.primaryErrorMessage);
    QCOMPARE(actualInfo.retryMode, expectedInfo.retryMode);
    QCOMPARE(actualInfo.subject, expectedInfo.subject);
    QCOMPARE(actualInfo.resultName, expectedInfo.resultName);
    QCOMPARE(actualInfo.message, expectedInfo.message);
    QCOMPARE(actualInfo.completed, expectedInfo.completed);
    QCOMPARE(actualInfo.total, expectedInfo.total);
    QCOMPARE(actualInfo.confirmedBytes, expectedInfo.confirmedBytes);
    QCOMPARE(actualInfo.lastConfirmedChunkIndex,
             expectedInfo.lastConfirmedChunkIndex);
    QCOMPARE(actualInfo.attempt, expectedInfo.attempt);
    QCOMPARE(actualInfo.deviceGeneration, expectedInfo.deviceGeneration);
    QCOMPARE(actualInfo.applyAfterUpload, expectedInfo.applyAfterUpload);
}

void PrinterProtocolTests::runtimeMediaCatalogDbusRoundTrip() {
    registerTryxRuntimeMetaTypes();

    TryxRuntimeMediaEntry entry;
    entry.name = QStringLiteral("uploaded.mp4.h264_2240x1080");
    entry.size = 6125181;
    entry.source = 1;
    entry.readOnly = false;
    entry.thumbnailKey = QString(64, QLatin1Char('a'));
    entry.managedOrigin = true;
    entry.deleteAllowed = true;
    entry.deleteBlockReason = QStringLiteral("ManagedUserMedia");
    TryxRuntimeMediaCatalogSnapshot expected;
    expected.revision = 9;
    expected.deviceIdentity = QStringLiteral("PASE-001");
    expected.entries.append(entry);

    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.isConnected(), qPrintable(bus.lastError().message()));
    RuntimeRoundTripObject serviceObject;
    const QString objectPath =
        QStringLiteral("/org/tryx/Panorama/MediaTest/%1")
            .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus.registerObject(objectPath, &serviceObject,
                                QDBusConnection::ExportAllSlots),
             qPrintable(bus.lastError().message()));
    QDBusInterface interface(bus.baseService(), objectPath,
                             QStringLiteral("org.tryx.Panorama.Test"), bus);
    const QDBusReply<TryxRuntimeMediaCatalogSnapshot> reply = interface.call(
        QStringLiteral("EchoMediaCatalog"), QVariant::fromValue(expected));
    bus.unregisterObject(objectPath);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    const TryxRuntimeMediaCatalogSnapshot actual = reply.value();
    QCOMPARE(actual.revision, expected.revision);
    QCOMPARE(actual.deviceIdentity, expected.deviceIdentity);
    QCOMPARE(actual.entries.size(), 1);
    QCOMPARE(actual.entries.first().name, entry.name);
    QCOMPARE(actual.entries.first().size, entry.size);
    QCOMPARE(actual.entries.first().source, entry.source);
    QCOMPARE(actual.entries.first().readOnly, entry.readOnly);
    QCOMPARE(actual.entries.first().thumbnailKey, entry.thumbnailKey);
    QCOMPARE(actual.entries.first().managedOrigin, entry.managedOrigin);
    QCOMPARE(actual.entries.first().deleteAllowed, entry.deleteAllowed);
    QCOMPARE(actual.entries.first().deleteBlockReason,
             entry.deleteBlockReason);
}

void PrinterProtocolTests::runtimeLegacyMediaCatalogDbusRoundTrip() {
    registerTryxRuntimeMetaTypes();

    TryxRuntimeLegacyMediaEntry entry;
    entry.name = QStringLiteral("legacy.mp4.h264_2240x1080");
    entry.size = 42;
    entry.source = 1;
    entry.readOnly = false;
    entry.thumbnailKey = QString(64, QLatin1Char('c'));
    TryxRuntimeLegacyMediaCatalogSnapshot expected;
    expected.revision = 8;
    expected.deviceIdentity = QStringLiteral("PASE-LEGACY");
    expected.entries.append(entry);

    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.isConnected(), qPrintable(bus.lastError().message()));
    RuntimeRoundTripObject serviceObject;
    const QString objectPath =
        QStringLiteral("/org/tryx/Panorama/LegacyMediaTest/%1")
            .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus.registerObject(objectPath, &serviceObject,
                                QDBusConnection::ExportAllSlots),
             qPrintable(bus.lastError().message()));
    QDBusInterface interface(bus.baseService(), objectPath,
                             QStringLiteral("org.tryx.Panorama.Test"), bus);
    const QDBusReply<TryxRuntimeLegacyMediaCatalogSnapshot> reply =
        interface.call(QStringLiteral("EchoLegacyMediaCatalog"),
                       QVariant::fromValue(expected));
    bus.unregisterObject(objectPath);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    const auto actual = reply.value();
    QCOMPARE(actual.revision, expected.revision);
    QCOMPARE(actual.deviceIdentity, expected.deviceIdentity);
    QCOMPARE(actual.entries.size(), 1);
    QCOMPARE(actual.entries.first().name, entry.name);
    QCOMPARE(actual.entries.first().size, entry.size);
    QCOMPARE(actual.entries.first().source, entry.source);
    QCOMPARE(actual.entries.first().readOnly, entry.readOnly);
    QCOMPARE(actual.entries.first().thumbnailKey, entry.thumbnailKey);
}

void PrinterProtocolTests::runtimeMetricsDbusRoundTrip() {
    registerTryxRuntimeMetaTypes();
    QCOMPARE(tryxRuntimeApiVersion(), 6U);

    TryxRuntimeMetricsConfigRequest expectedRequest;
    expectedRequest.enabled = true;
    expectedRequest.metrics = {
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Temperature"),
        QStringLiteral("Date&Time")};
    expectedRequest.alignment = QStringLiteral("Right");
    expectedRequest.textColor = 0x000000U;

    TryxRuntimeMetricsState expectedState;
    expectedState.revision = 27;
    expectedState.deviceSerial = QStringLiteral("PASE-METRICS-001");
    expectedState.enabled = true;
    expectedState.samplingActive = true;
    expectedState.metrics = expectedRequest.metrics;
    expectedState.availableMetrics = {
        QStringLiteral("CPU Power"), QStringLiteral("Date&Time")};
    expectedState.alignment = expectedRequest.alignment;
    expectedState.textColor = expectedRequest.textColor;
    expectedState.diagnostic = QStringLiteral("sensor status");

    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.isConnected(), qPrintable(bus.lastError().message()));
    RuntimeRoundTripObject serviceObject;
    const QString objectPath =
        QStringLiteral("/org/tryx/Panorama/MetricsTest/%1")
            .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus.registerObject(objectPath, &serviceObject,
                                QDBusConnection::ExportAllSlots),
             qPrintable(bus.lastError().message()));
    QDBusInterface interface(bus.baseService(), objectPath,
                             QStringLiteral("org.tryx.Panorama.Test"), bus);

    const QDBusReply<TryxRuntimeMetricsConfigRequest> requestReply =
        interface.call(QStringLiteral("EchoMetricsRequest"),
                       QVariant::fromValue(expectedRequest));
    QVERIFY2(requestReply.isValid(),
             qPrintable(requestReply.error().message()));
    const TryxRuntimeMetricsConfigRequest actualRequest =
        requestReply.value();
    QCOMPARE(actualRequest.enabled, expectedRequest.enabled);
    QCOMPARE(actualRequest.metrics, expectedRequest.metrics);
    QCOMPARE(actualRequest.alignment, expectedRequest.alignment);
    QCOMPARE(actualRequest.textColor, expectedRequest.textColor);

    const QDBusReply<TryxRuntimeMetricsState> stateReply =
        interface.call(QStringLiteral("EchoMetricsState"),
                       QVariant::fromValue(expectedState));
    bus.unregisterObject(objectPath);
    QVERIFY2(stateReply.isValid(), qPrintable(stateReply.error().message()));
    const TryxRuntimeMetricsState actualState = stateReply.value();
    QCOMPARE(actualState.revision, expectedState.revision);
    QCOMPARE(actualState.deviceSerial, expectedState.deviceSerial);
    QCOMPARE(actualState.enabled, expectedState.enabled);
    QCOMPARE(actualState.samplingActive, expectedState.samplingActive);
    QCOMPARE(actualState.metrics, expectedState.metrics);
    QCOMPARE(actualState.availableMetrics, expectedState.availableMetrics);
    QCOMPARE(actualState.alignment, expectedState.alignment);
    QCOMPARE(actualState.textColor, expectedState.textColor);
    QCOMPARE(actualState.diagnostic, expectedState.diagnostic);
}

void PrinterProtocolTests::runtimeDisplayConfigDbusRoundTrip() {
    registerTryxRuntimeMetaTypes();

    TryxRuntimeApplyRequest expectedRequest;
    expectedRequest.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    expectedRequest.ratio = QStringLiteral("2:1");
    expectedRequest.screenMode =
        QStringLiteral("Screen Splitting");
    expectedRequest.playMode = QStringLiteral("Single");
    expectedRequest.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};
    expectedRequest.settingsPosition = QStringLiteral("Top");
    expectedRequest.settingsColor = QStringLiteral("#DCDCDC");
    expectedRequest.settingsAlign = QStringLiteral("Left");
    expectedRequest.settingsBadges = {
        QStringLiteral("CPU Badge")};
    expectedRequest.filterOpacity = 33;
    expectedRequest.presetId = QStringLiteral("preset");
    expectedRequest.sysinfoLabels2 = {
        QStringLiteral("GPU Power")};
    expectedRequest.settingsBadges2 = {
        QStringLiteral("GPU Badge")};
    expectedRequest.settingsPosition2 = QStringLiteral("Bottom");
    expectedRequest.settingsColor2 = QStringLiteral("#000000");
    expectedRequest.settingsAlign2 = QStringLiteral("Right");
    expectedRequest.waterfallMode = true;
    expectedRequest.replaceOverlay = true;
    expectedRequest.display.brightnessPresent = true;
    expectedRequest.display.brightness = 64;
    expectedRequest.display.standbyPresent = true;
    expectedRequest.display.standbyEnabled = false;
    expectedRequest.display.orientationPresent = true;
    expectedRequest.display.mirrorMode = true;
    expectedRequest.display.waterfallMode = true;
    expectedRequest.display.backlightPresent = true;
    expectedRequest.display.backlightEnabled = false;

    TryxRuntimeDisplayState expectedState;
    expectedState.revision = 41;
    expectedState.deviceSerial = QStringLiteral("PASE-DISPLAY-001");
    expectedState.valid = true;
    expectedState.backlightEnabled = true;
    expectedState.brightness = 64;
    expectedState.standbyEnabled = false;
    expectedState.standbyMedia = QStringLiteral("standby.h264");
    expectedState.mirrorMode = true;
    expectedState.waterfallMode = true;
    expectedState.screenMode = expectedRequest.screenMode;
    expectedState.playMode = expectedRequest.playMode;
    expectedState.media = expectedRequest.media;
    expectedState.sysinfoLabels = expectedRequest.sysinfoLabels;
    expectedState.settingsBadges = expectedRequest.settingsBadges;
    expectedState.settingsPosition =
        expectedRequest.settingsPosition;
    expectedState.settingsColor = expectedRequest.settingsColor;
    expectedState.settingsAlign = expectedRequest.settingsAlign;
    expectedState.sysinfoLabels2 =
        expectedRequest.sysinfoLabels2;
    expectedState.settingsBadges2 =
        expectedRequest.settingsBadges2;
    expectedState.settingsPosition2 =
        expectedRequest.settingsPosition2;
    expectedState.settingsColor2 =
        expectedRequest.settingsColor2;
    expectedState.settingsAlign2 =
        expectedRequest.settingsAlign2;
    expectedState.diagnostic = QStringLiteral("display status");

    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.isConnected(), qPrintable(bus.lastError().message()));
    RuntimeRoundTripObject serviceObject;
    const QString objectPath =
        QStringLiteral("/org/tryx/Panorama/DisplayTest/%1")
            .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus.registerObject(objectPath, &serviceObject,
                                QDBusConnection::ExportAllSlots),
             qPrintable(bus.lastError().message()));
    QDBusInterface interface(bus.baseService(), objectPath,
                             QStringLiteral("org.tryx.Panorama.Test"), bus);

    const QDBusReply<TryxRuntimeApplyRequest> requestReply =
        interface.call(QStringLiteral("EchoApplyRequest"),
                       QVariant::fromValue(expectedRequest));
    QVERIFY2(requestReply.isValid(),
             qPrintable(requestReply.error().message()));
    const TryxRuntimeApplyRequest actualRequest = requestReply.value();
    QCOMPARE(actualRequest.media, expectedRequest.media);
    QCOMPARE(actualRequest.ratio, expectedRequest.ratio);
    QCOMPARE(actualRequest.screenMode, expectedRequest.screenMode);
    QCOMPARE(actualRequest.playMode, expectedRequest.playMode);
    QCOMPARE(actualRequest.sysinfoLabels,
             expectedRequest.sysinfoLabels);
    QCOMPARE(actualRequest.settingsPosition,
             expectedRequest.settingsPosition);
    QCOMPARE(actualRequest.settingsColor,
             expectedRequest.settingsColor);
    QCOMPARE(actualRequest.settingsAlign,
             expectedRequest.settingsAlign);
    QCOMPARE(actualRequest.settingsBadges,
             expectedRequest.settingsBadges);
    QCOMPARE(actualRequest.filterOpacity,
             expectedRequest.filterOpacity);
    QCOMPARE(actualRequest.presetId, expectedRequest.presetId);
    QCOMPARE(actualRequest.sysinfoLabels2,
             expectedRequest.sysinfoLabels2);
    QCOMPARE(actualRequest.settingsBadges2,
             expectedRequest.settingsBadges2);
    QCOMPARE(actualRequest.settingsPosition2,
             expectedRequest.settingsPosition2);
    QCOMPARE(actualRequest.settingsColor2,
             expectedRequest.settingsColor2);
    QCOMPARE(actualRequest.settingsAlign2,
             expectedRequest.settingsAlign2);
    QCOMPARE(actualRequest.waterfallMode,
             expectedRequest.waterfallMode);
    QCOMPARE(actualRequest.replaceOverlay,
             expectedRequest.replaceOverlay);
    QCOMPARE(actualRequest.display.brightnessPresent,
             expectedRequest.display.brightnessPresent);
    QCOMPARE(actualRequest.display.brightness,
             expectedRequest.display.brightness);
    QCOMPARE(actualRequest.display.standbyPresent,
             expectedRequest.display.standbyPresent);
    QCOMPARE(actualRequest.display.standbyEnabled,
             expectedRequest.display.standbyEnabled);
    QCOMPARE(actualRequest.display.orientationPresent,
             expectedRequest.display.orientationPresent);
    QCOMPARE(actualRequest.display.mirrorMode,
             expectedRequest.display.mirrorMode);
    QCOMPARE(actualRequest.display.waterfallMode,
             expectedRequest.display.waterfallMode);
    QCOMPARE(actualRequest.display.backlightPresent,
             expectedRequest.display.backlightPresent);
    QCOMPARE(actualRequest.display.backlightEnabled,
             expectedRequest.display.backlightEnabled);

    const QDBusReply<TryxRuntimeDisplayState> stateReply =
        interface.call(QStringLiteral("EchoDisplayState"),
                       QVariant::fromValue(expectedState));
    bus.unregisterObject(objectPath);
    QVERIFY2(stateReply.isValid(),
             qPrintable(stateReply.error().message()));
    const TryxRuntimeDisplayState actualState = stateReply.value();
    QCOMPARE(actualState.revision, expectedState.revision);
    QCOMPARE(actualState.deviceSerial, expectedState.deviceSerial);
    QCOMPARE(actualState.valid, expectedState.valid);
    QCOMPARE(actualState.backlightEnabled,
             expectedState.backlightEnabled);
    QCOMPARE(actualState.brightness, expectedState.brightness);
    QCOMPARE(actualState.standbyEnabled,
             expectedState.standbyEnabled);
    QCOMPARE(actualState.standbyMedia, expectedState.standbyMedia);
    QCOMPARE(actualState.mirrorMode, expectedState.mirrorMode);
    QCOMPARE(actualState.waterfallMode, expectedState.waterfallMode);
    QCOMPARE(actualState.screenMode, expectedState.screenMode);
    QCOMPARE(actualState.playMode, expectedState.playMode);
    QCOMPARE(actualState.media, expectedState.media);
    QCOMPARE(actualState.sysinfoLabels,
             expectedState.sysinfoLabels);
    QCOMPARE(actualState.settingsBadges,
             expectedState.settingsBadges);
    QCOMPARE(actualState.settingsPosition,
             expectedState.settingsPosition);
    QCOMPARE(actualState.settingsColor,
             expectedState.settingsColor);
    QCOMPARE(actualState.settingsAlign,
             expectedState.settingsAlign);
    QCOMPARE(actualState.sysinfoLabels2,
             expectedState.sysinfoLabels2);
    QCOMPARE(actualState.settingsBadges2,
             expectedState.settingsBadges2);
    QCOMPARE(actualState.settingsPosition2,
             expectedState.settingsPosition2);
    QCOMPARE(actualState.settingsColor2,
             expectedState.settingsColor2);
    QCOMPARE(actualState.settingsAlign2,
             expectedState.settingsAlign2);
    QCOMPARE(actualState.diagnostic, expectedState.diagnostic);
}

void PrinterProtocolTests::
remoteDisplayStateRequiresStrictlyIncreasingRevision() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->remoteMode_ = true;
    manager->remoteApiCompatible_ = true;
    QSignalSpy displaySpy(
        manager.get(), &DeviceManager::displayStateUpdated);

    TryxRuntimeDisplayState initial;
    initial.revision = 0;
    initial.valid = true;
    initial.brightness = 41;
    manager->handleRemoteDisplayStateUpdated(initial);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(manager->displayState().brightness, 41);

    TryxRuntimeDisplayState duplicate = initial;
    duplicate.brightness = 99;
    manager->handleRemoteDisplayStateUpdated(duplicate);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(manager->displayState().brightness, 41);

    TryxRuntimeDisplayState newer = initial;
    newer.revision = 1;
    newer.brightness = 73;
    manager->handleRemoteDisplayStateUpdated(newer);
    QCOMPARE(displaySpy.count(), 2);
    QCOMPARE(manager->displayState().brightness, 73);
    manager->remoteMode_ = false;
}

void PrinterProtocolTests::
panoramaPageRestoresDisplayAndSplitState() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    PanoramaPage page(manager.get());

    auto *brightness = page.findChild<QSlider *>(
        QStringLiteral("displayBrightnessSlider"));
    auto *displayOff = page.findChild<QCheckBox *>(
        QStringLiteral("displayOffCheckBox"));
    auto *mirror = page.findChild<QCheckBox *>(
        QStringLiteral("displayMirrorCheckBox"));
    auto *waterfall = page.findChild<QCheckBox *>(
        QStringLiteral("displayWaterfallCheckBox"));
    auto *fullScreen = page.findChild<QRadioButton *>(
        QStringLiteral("fullScreenRadioButton"));
    auto *splitScreen = page.findChild<QRadioButton *>(
        QStringLiteral("splitScreenRadioButton"));
    auto *cpuBadge = page.findChild<QCheckBox *>(
        QStringLiteral("customCpuBadgeCheckBox"));
    auto *gpuBadge = page.findChild<QCheckBox *>(
        QStringLiteral("customGpuBadgeCheckBox"));
    auto *colorButton = page.findChild<QPushButton *>(
        QStringLiteral("customTextColorButton"));
    auto *splitConfig =
        page.findChild<SplitConfigWidget *>();
    QVERIFY(brightness);
    QVERIFY(displayOff);
    QVERIFY(mirror);
    QVERIFY(waterfall);
    QVERIFY(fullScreen);
    QVERIFY(splitScreen);
    QVERIFY(cpuBadge);
    QVERIFY(gpuBadge);
    QVERIFY(colorButton);
    QVERIFY(splitConfig);

    TryxRuntimeDisplayState state;
    state.revision = 7;
    state.deviceSerial = QStringLiteral("PASE-UI");
    state.valid = true;
    state.backlightEnabled = false;
    state.brightness = 64;
    state.standbyEnabled = false;
    state.standbyMedia =
        QStringLiteral("screensaver.h264");
    state.mirrorMode = true;
    state.waterfallMode = true;
    state.screenMode =
        QStringLiteral("Screen Splitting");
    state.playMode = QStringLiteral("Single");
    state.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    state.sysinfoLabels = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Power")};
    state.settingsBadges = {
        QStringLiteral("CPU Badge")};
    state.settingsPosition = QStringLiteral("Bottom");
    state.settingsColor = QStringLiteral("#ff0000");
    state.settingsAlign = QStringLiteral("Right");
    state.sysinfoLabels2 = {
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Power")};
    state.settingsBadges2 = {
        QStringLiteral("GPU Badge")};
    state.settingsPosition2 = QStringLiteral("Top");
    state.settingsColor2 = QStringLiteral("#00ff00");
    state.settingsAlign2 = QStringLiteral("Left");

    manager->displayStateUpdated(state);
    QCoreApplication::processEvents();

    QCOMPARE(brightness->value(), 64);
    QVERIFY(!brightness->isEnabled());
    QSignalSpy legacyBrightnessSpy(
        manager.get(), &DeviceManager::requestBrightness);
    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 88)));
    QCoreApplication::processEvents();
    QCOMPARE(legacyBrightnessSpy.count(), 0);
    QVERIFY(displayOff->isChecked());
    QVERIFY(mirror->isChecked());
    QVERIFY(waterfall->isChecked());
    QVERIFY(!fullScreen->isChecked());
    QVERIFY(splitScreen->isChecked());
    QVERIFY(cpuBadge->isChecked());
    QVERIFY(!gpuBadge->isChecked());
    QVERIFY(colorButton->styleSheet().contains(
        QStringLiteral("#ff0000"),
        Qt::CaseInsensitive));
    QCOMPARE(splitConfig->leftMedia(),
             QStringList{QStringLiteral("left.h264")});
    QCOMPARE(splitConfig->rightMedia(),
             QStringList{QStringLiteral("right.h264")});
    QCOMPARE(splitConfig->leftMetrics(),
             state.sysinfoLabels);
    QCOMPARE(splitConfig->rightMetrics(),
             state.sysinfoLabels2);
    QCOMPARE(splitConfig->leftBadges(),
             state.settingsBadges);
    QCOMPARE(splitConfig->rightBadges(),
             state.settingsBadges2);
    QCOMPARE(splitConfig->leftPosition(),
             QStringLiteral("Bottom"));
    QCOMPARE(splitConfig->rightPosition(),
             QStringLiteral("Top"));
    QCOMPARE(splitConfig->leftColor(),
             QStringLiteral("#ff0000"));
    QCOMPARE(splitConfig->rightColor(),
             QStringLiteral("#00ff00"));
    QCOMPARE(splitConfig->leftAlignment(),
             QStringLiteral("Right"));
    QCOMPARE(splitConfig->rightAlignment(),
             QStringLiteral("Left"));
    QCOMPARE(splitConfig->playMode(),
             QStringLiteral("Single"));
}

void PrinterProtocolTests::
panoramaPageOmitsPresetSurface() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    PanoramaPage page(manager.get());
    QVERIFY(page.findChildren<QTabBar *>().isEmpty());
    QVERIFY(!page.findChild<QWidget *>(
        QStringLiteral("presetCpuBadgeCheckBox")));
    QVERIFY(page.findChild<QCheckBox *>(
        QStringLiteral("customCpuBadgeCheckBox")));

    const QString presetId =
        QStringLiteral("device-preset.h264");
    PrinterProtocol::MediaFile presetEntry;
    presetEntry.name = presetId;
    presetEntry.source = PrinterProtocol::MediaSource::Preset;
    presetEntry.readOnly = true;
    PrinterProtocol::MediaFile userEntry = presetEntry;
    userEntry.name = QStringLiteral("user-upload.h264");
    userEntry.source = PrinterProtocol::MediaSource::User;
    userEntry.readOnly = false;
    manager->updateMediaCatalog({presetEntry, userEntry});

    const TryxRuntimeMediaCatalogSnapshot catalog =
        manager->mediaCatalogSnapshot();
    QCOMPARE(catalog.entries.size(), 2);
    const auto userCatalogEntry = std::find_if(
        catalog.entries.cbegin(), catalog.entries.cend(),
        [&userEntry](const TryxRuntimeMediaEntry &entry) {
            return entry.name == userEntry.name;
        });
    QVERIFY(userCatalogEntry != catalog.entries.cend());
    QCOMPARE(page.fileList_->count(), 1);
    QListWidgetItem *visibleItem = page.fileList_->item(0);
    QVERIFY(visibleItem);
    QCOMPARE(
        visibleItem->data(Qt::UserRole).toString(),
        userEntry.name);
    constexpr int mediaSourceRole = Qt::UserRole + 2;
    QCOMPARE(
        visibleItem->data(mediaSourceRole).toUInt(),
        userCatalogEntry->source);
}

void PrinterProtocolTests::
panoramaBrightnessCoalescesUntilTransportReady() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("dev"));
    QVERIFY(createUsbDevice(
        sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(
        sysRoot, devRoot, QStringLiteral("1-1"),
        QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestPrinterApplyMedia,
        manager->worker_, &DeviceWorker::applyPrinterMedia);
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_,
        &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    manager->printerDisplaySessionLost_ = false;
    manager->displayState_.revision = 10;
    manager->displayState_.deviceSerial =
        QStringLiteral("PASE-BRIGHTNESS");
    manager->displayState_.valid = true;
    manager->displayState_.backlightEnabled = true;
    manager->displayState_.brightness = 50;
    manager->displayState_.standbyEnabled = true;
    manager->displayState_.standbyMedia =
        QStringLiteral("standby.h264");
    manager->displayState_.screenMode =
        QStringLiteral("Full Screen");
    manager->displayState_.playMode =
        QStringLiteral("Single");

    PanoramaPage page(manager.get());
    auto *slider = page.findChild<QSlider *>(
        QStringLiteral("displayBrightnessSlider"));
    QVERIFY(slider);
    QSignalSpy applySpy(
        manager.get(),
        &DeviceManager::requestPrinterApplyMedia);
    QSignalSpy transportReadySpy(
        manager.get(),
        &DeviceManager::printerTransportReady);

    QVERIFY(!page.displayMutationReady_);
    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(transportReadySpy.count(), 1);
    QVERIFY(page.displayMutationReady_);

    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 60)));
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 1);
    const QString firstOperation =
        page.brightnessOperationId_;
    QVERIFY(!firstOperation.isEmpty());
    const TryxRuntimeApplyRequest firstRequest =
        qvariant_cast<TryxRuntimeApplyRequest>(
            applySpy.at(0).at(2));
    QCOMPARE(firstRequest.display.brightness, 60);

    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 70)));
    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 80)));
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(page.pendingBrightness_, 80);
    QCOMPARE(slider->value(), 80);
    emit manager->brightnessChanged(60);
    QCoreApplication::processEvents();
    QCOMPARE(slider->value(), 80);

    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(transportReadySpy.count(), 1);
    QVERIFY(!page.displayMutationReady_);
    emit manager->printerTransportReady();
    QCoreApplication::processEvents();
    QVERIFY(!page.displayMutationReady_);

    manager->displayState_.revision = 11;
    manager->displayState_.brightness = 60;
    emit manager->displayStateUpdated(
        manager->displayState_);
    manager->worker_->printerApplyFinished(
        firstOperation, QString(), true, false,
        PrinterProtocol::MutationOutcome::Succeeded,
        QString(), manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 1);
    QVERIFY(page.brightnessOperationId_.isEmpty());
    QCOMPARE(page.pendingBrightness_, 80);

    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 2);
    const QString secondOperation =
        page.brightnessOperationId_;
    QVERIFY(!secondOperation.isEmpty());
    const TryxRuntimeApplyRequest secondRequest =
        qvariant_cast<TryxRuntimeApplyRequest>(
            applySpy.at(1).at(2));
    QCOMPARE(secondRequest.display.brightness, 80);

    manager->displayState_.revision = 12;
    manager->displayState_.brightness = 80;
    emit manager->displayStateUpdated(
        manager->displayState_);
    manager->worker_->printerApplyFinished(
        secondOperation, QString(), true, false,
        PrinterProtocol::MutationOutcome::Succeeded,
        QString(), manager->printerGeneration_);
    QCoreApplication::processEvents();
    QVERIFY(page.brightnessOperationId_.isEmpty());
    QVERIFY(!page.displayMutationReady_);

    manager->printerDisplaySessionActive_ = false;
    emit manager->printerDisplaySessionChanged(false);
    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    emit manager->printerTransportReady();
    QCoreApplication::processEvents();
    QVERIFY(!page.displayMutationReady_);
    manager->printerDisplaySessionActive_ = true;
    emit manager->printerDisplaySessionChanged(true);

    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 90)));
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 2);
    QCOMPARE(page.pendingBrightness_, 90);
    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 3);
    const QString failedOperation =
        page.brightnessOperationId_;
    QVERIFY(!failedOperation.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(
        &page, "onBrightnessChanged",
        Qt::DirectConnection, Q_ARG(int, 95)));
    QCoreApplication::processEvents();
    QCOMPARE(page.pendingBrightness_, 95);

    manager->worker_->printerApplyFinished(
        failedOperation, QString(), false, false,
        PrinterProtocol::MutationOutcome::PartialOrUnknown,
        QStringLiteral("unknown"), manager->printerGeneration_);
    QCoreApplication::processEvents();
    emit manager->worker_->printerTransportReady(
        manager->printerGeneration_);
    QCoreApplication::processEvents();
    QCOMPARE(applySpy.count(), 3);
    QCOMPARE(page.pendingBrightness_, -1);
    QVERIFY(page.brightnessOperationId_.isEmpty());
    QCOMPARE(slider->value(), 80);
}

void PrinterProtocolTests::paseRunConfigUsesWireLayout() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    PrinterProtocol protocol;
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("/dev/usb/lp-pase-layout"));
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        if (!readRequest(sockets[1], &captured, &peerError)) {
            return;
        }
        auto response = baseResponse(captured);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {QStringLiteral("CPU Temperature")};
    overlay.left.alignment = QStringLiteral("Left");
    overlay.left.textColor = 0xFF0000U;
    QString error;
    const bool sent = protocol.sendPaseRunConfigForTesting(
        QStringLiteral("/dev/usb/lp-pase-layout"), overlay, &error,
        PrinterProtocol::OperationContext{});
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(sent, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(captured.body_case(),
             panorama::wire::v1::Request::kOverlayLayout);
    QCOMPARE(captured.overlay_layout().label_groups_size(), 1);
    const auto &group = captured.overlay_layout().label_groups(0);
    QCOMPARE(group.group_id(), 100U);
    QCOMPARE(group.group_x(), 60U);
    QCOMPARE(group.group_y(), 440U);
    QCOMPARE(group.group_width(), 2120U);
    QCOMPARE(group.group_height(), 160U);
    QCOMPARE(group.text_align(), panorama::wire::v1::OverlayGroup::ALIGN_LEFT);
    QCOMPARE(group.line_gap(), -10);
    QCOMPARE(group.labels_size(), 3);
    QCOMPARE(group.labels(0).label_id(), 101U);
    QCOMPARE(group.labels(0).line(), 1U);
    QCOMPARE(group.labels(0).gap_left(), 13);
    QCOMPARE(group.labels(0).text_size(), 30U);
    QCOMPARE(group.labels(0).text_color(), 0xFF0000U);
    QCOMPARE(QString::fromStdString(group.labels(0).text()),
             QStringLiteral("CPU TEMP"));
    QCOMPARE(group.labels(1).label_id(), 102U);
    QCOMPARE(group.labels(1).text_size(), 160U);
    QCOMPARE(QString::fromStdString(group.labels(1).text()),
             QStringLiteral("--"));
    QCOMPARE(group.labels(2).label_id(), 103U);
    QCOMPARE(group.labels(2).text_size(), 36U);
    QCOMPARE(QString::fromStdString(group.labels(2).text()),
             QStringLiteral("°C"));
}

void PrinterProtocolTests::
paseWaterfallFullScreenGeometry_data() {
    QTest::addColumn<QString>("placement");
    QTest::addColumn<quint32>("metricY");
    QTest::addColumn<quint32>("badgeY");

    QTest::newRow("top")
        << QStringLiteral("Top") << 440U << 70U;
    QTest::newRow("bottom")
        << QStringLiteral("Bottom") << 1560U << 1190U;
}

void PrinterProtocolTests::
paseWaterfallFullScreenGeometry() {
    QFETCH(QString, placement);
    QFETCH(quint32, metricY);
    QFETCH(quint32, badgeY);

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    PrinterProtocol protocol;
    const QString endpoint =
        QStringLiteral("/dev/usb/lp-pase-waterfall-full");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], endpoint);
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        if (!readRequest(
                sockets[1], &captured, &peerError)) {
            return;
        }
        auto response = baseResponse(captured);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.waterfallMode = true;
    overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    overlay.left.badges = {
        QStringLiteral("CPU Badge")};
    overlay.left.verticalPlacement = placement;
    overlay.left.alignment = QStringLiteral("Right");
    overlay.left.textColor = 0xFF0000U;
    overlay.cpuBadgeText =
        QStringLiteral("AMD Ryzen 9 9950X3D");
    QString error;
    const bool sent = protocol.sendPaseRunConfigForTesting(
        endpoint, overlay, &error,
        PrinterProtocol::OperationContext{});
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(sent, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(captured.overlay_layout().label_groups_size(), 2);
    const auto &metric =
        captured.overlay_layout().label_groups(0);
    const auto &badge =
        captured.overlay_layout().label_groups(1);
    QCOMPARE(metric.group_id(), 100U);
    QCOMPARE(metric.group_x(), 60U);
    QCOMPARE(metric.group_y(), metricY);
    QCOMPARE(metric.group_width(), 950U);
    QCOMPARE(metric.group_height(), 160U);
    QCOMPARE(metric.text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_RIGHT);
    QCOMPARE(metric.labels(0).text_color(),
             0xFF0000U);
    QCOMPARE(badge.group_id(), 300U);
    QCOMPARE(badge.group_x(), 70U);
    QCOMPARE(badge.group_y(), badgeY);
    QCOMPARE(badge.group_width(), 970U);
    QCOMPARE(badge.text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_RIGHT);
}

void PrinterProtocolTests::
paseWaterfallSplitGeometryAndIndependentStyles() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    PrinterProtocol protocol;
    const QString endpoint =
        QStringLiteral("/dev/usb/lp-pase-waterfall-split");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], endpoint);
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        if (!readRequest(
                sockets[1], &captured, &peerError)) {
            return;
        }
        auto response = baseResponse(captured);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.dualMode = true;
    overlay.waterfallMode = true;
    overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    overlay.left.badges = {
        QStringLiteral("CPU Badge")};
    overlay.left.verticalPlacement =
        QStringLiteral("Bottom");
    overlay.left.alignment = QStringLiteral("Right");
    overlay.left.textColor = 0xFF0000U;
    overlay.right.metrics = {
        QStringLiteral("GPU Power")};
    overlay.right.badges = {
        QStringLiteral("GPU Badge")};
    overlay.right.verticalPlacement =
        QStringLiteral("Top");
    overlay.right.alignment = QStringLiteral("Left");
    overlay.right.textColor = 0x00FF00U;
    overlay.cpuBadgeText =
        QStringLiteral("AMD Ryzen 9 9950X3D");
    overlay.gpuBadgeText =
        QStringLiteral("AMD Radeon RX 7900 XTX");
    QString error;
    const bool sent = protocol.sendPaseRunConfigForTesting(
        endpoint, overlay, &error,
        PrinterProtocol::OperationContext{});
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(sent, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    const auto &run = captured.overlay_layout();
    QCOMPARE(run.label_groups_size(), 4);
    const auto findGroup = [&run](quint32 groupId)
        -> const panorama::wire::v1::OverlayGroup * {
        for (int index = 0;
             index < run.label_groups_size(); ++index) {
            if (run.label_groups(index).group_id() ==
                groupId) {
                return &run.label_groups(index);
            }
        }
        return nullptr;
    };
    const auto *leftMetric = findGroup(100);
    const auto *leftBadge = findGroup(300);
    const auto *rightMetric = findGroup(207);
    const auto *rightBadge = findGroup(400);
    QVERIFY(leftMetric);
    QVERIFY(leftBadge);
    QVERIFY(rightMetric);
    QVERIFY(rightBadge);

    QCOMPARE(leftMetric->group_x(), 60U);
    QCOMPARE(leftMetric->group_y(), 1560U);
    QCOMPARE(leftMetric->group_width(), 950U);
    QCOMPARE(leftMetric->text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_RIGHT);
    QCOMPARE(leftMetric->labels(0).text_color(),
             0xFF0000U);
    QCOMPARE(leftBadge->group_x(), 70U);
    QCOMPARE(leftBadge->group_y(), 1190U);
    QCOMPARE(leftBadge->group_width(), 970U);
    QCOMPARE(leftBadge->text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_RIGHT);

    QCOMPARE(rightMetric->group_x(), 60U);
    QCOMPARE(rightMetric->group_y(), 440U);
    QCOMPARE(rightMetric->group_width(), 950U);
    QCOMPARE(rightMetric->text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_LEFT);
    QCOMPARE(rightMetric->labels(0).text_color(),
             0x00FF00U);
    QCOMPARE(rightBadge->group_x(), 70U);
    QCOMPARE(rightBadge->group_y(), 70U);
    QCOMPARE(rightBadge->group_width(), 970U);
    QCOMPARE(rightBadge->text_align(),
             panorama::wire::v1::OverlayGroup::ALIGN_LEFT);
}

void PrinterProtocolTests::paseSplitApplyBuildsDualUserConfigAndBadges() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    panorama::wire::v1::Request capturedUserConfig;
    panorama::wire::v1::Request capturedRunConfig;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "split apply did not read user config");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *userConfig = getResponse.mutable_user_configuration();
        userConfig->mutable_display_config()
            ->set_backlight_brightness(55);
        userConfig->mutable_work_config()
            ->set_single_mode_media_file("old.h264");
        userConfig->mutable_standby_config()
            ->set_media_file("standby.h264");
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        if (!readRequest(sockets[1], &capturedUserConfig,
                         &peerError) ||
            capturedUserConfig.body_case() !=
                panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral(
                "split apply did not send user config");
            return;
        }
        auto userResponse = baseResponse(capturedUserConfig);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], userResponse, &peerError)) {
            return;
        }

        if (!readRequest(sockets[1], &capturedRunConfig,
                         &peerError) ||
            capturedRunConfig.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "split apply did not send run config");
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "split apply did not verify user config");
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        *readbackResponse.mutable_user_configuration() =
            capturedUserConfig.user_configuration();
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-split");
    protocol.adoptFileDescriptorForTesting(sockets[0], devicePath);
    PrinterProtocol::PaseApplyConfig config;
    config.media = {QStringLiteral("left.h264"),
                    QStringLiteral("right.h264")};
    config.screenMode = QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    config.replaceOverlay = true;
    config.overlay.dualMode = true;
    config.overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    config.overlay.left.badges = {
        QStringLiteral("CPU Badge")};
    config.overlay.right.metrics = {
        QStringLiteral("GPU Power")};
    config.overlay.right.badges = {
        QStringLiteral("GPU Badge")};
    config.overlay.cpuBadgeText =
        QStringLiteral("AMD Ryzen 9 9950X3D");
    config.overlay.gpuBadgeText =
        QStringLiteral("NVIDIA GeForce RTX");
    QString error;
    PrinterProtocol::PaseDisplayState appliedState;
    const bool applied = protocol.applyPaseConfiguration(
        devicePath, config, &error,
        PrinterProtocol::OperationContext{}, nullptr,
        &appliedState);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(applied, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    const auto &work = capturedUserConfig.user_configuration().work_config();
    QCOMPARE(work.media_mode(),
             panorama::wire::v1::WorkConfiguration::MEDIA_DUAL);
    QCOMPARE(work.loop_mode(),
             panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
    QCOMPARE(QString::fromStdString(
                 work.dual_mode_left_media_file()),
             QStringLiteral("left.h264"));
    QCOMPARE(QString::fromStdString(
                 work.dual_mode_right_media_file()),
             QStringLiteral("right.h264"));
    QCOMPARE(appliedState.screenMode,
             QStringLiteral("Screen Splitting"));
    QCOMPARE(appliedState.media, config.media);

    const auto &run = capturedRunConfig.overlay_layout();
    QCOMPARE(run.label_groups_size(), 4);
    const auto findGroup = [&run](quint32 groupId)
        -> const panorama::wire::v1::OverlayGroup * {
        for (int index = 0; index < run.label_groups_size(); ++index) {
            if (run.label_groups(index).group_id() == groupId) {
                return &run.label_groups(index);
            }
        }
        return nullptr;
    };
    const auto *leftMetric = findGroup(100);
    const auto *leftBadge = findGroup(300);
    const auto *rightMetric = findGroup(207);
    const auto *rightBadge = findGroup(400);
    QVERIFY(leftMetric);
    QVERIFY(leftBadge);
    QVERIFY(rightMetric);
    QVERIFY(rightBadge);
    QCOMPARE(leftMetric->labels(0).label_id(), 101U);
    QCOMPARE(rightMetric->labels(0).label_id(), 222U);
    QCOMPARE(leftBadge->labels_size(), 1);
    QCOMPARE(leftBadge->labels(0).label_id(), 301U);
    QCOMPARE(leftBadge->labels(0).background(),
             panorama::wire::v1::OverlayLabel::BACKGROUND_GRADIENT_HORIZONTAL);
    QCOMPARE(leftBadge->labels(0).background_color(), 0x00A92F2CU);
    QCOMPARE(leftBadge->labels(0).gradient_color(), 0x00CB6236U);
    QCOMPARE(rightBadge->labels_size(), 1);
    QCOMPARE(rightBadge->labels(0).label_id(), 402U);
    QCOMPARE(rightBadge->labels(0).background_color(), 0x00629A00U);
    QCOMPARE(rightBadge->labels(0).gradient_color(), 0x0079AB51U);
}

void PrinterProtocolTests::
paseApplyRetriesDroppedReadOnlyConfigResponse_data() {
    QTest::addColumn<bool>("dropPreflightResponse");
    QTest::addColumn<bool>("sendLateDroppedResponse");
    QTest::addColumn<bool>("sendLateBeforeRetryRequest");

    QTest::newRow("preflight-clean-timeout")
        << true << false << false;
    QTest::newRow("preflight-late-after-retry")
        << true << true << false;
    QTest::newRow("preflight-late-during-backoff")
        << true << true << true;
    QTest::newRow("verification-clean-timeout")
        << false << false << false;
}

void PrinterProtocolTests::
paseApplyRetriesDroppedReadOnlyConfigResponse() {
    QFETCH(bool, dropPreflightResponse);
    QFETCH(bool, sendLateDroppedResponse);
    QFETCH(bool, sendLateBeforeRetryRequest);

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    quint64 droppedTrackId = 0;
    quint64 retryTrackId = 0;
    int userConfigurationWrites = 0;
    int overlayWrites = 0;
    QString peerError;
    std::thread peer([&]() {
        const auto writeInitialConfiguration =
            [&](const panorama::wire::v1::Request &request) {
                auto response = baseResponse(request);
                auto *configuration =
                    response.mutable_user_configuration();
                configuration->mutable_display_config()
                    ->set_backlight_brightness(55);
                auto *work =
                    configuration->mutable_work_config();
                work->set_media_mode(
                    panorama::wire::v1::WorkConfiguration::
                        MEDIA_SINGLE);
                work->set_loop_mode(
                    panorama::wire::v1::WorkConfiguration::
                        LOOP_SINGLE);
                work->set_single_mode_media_file(
                    "old.h264");
                return writeResponse(
                    sockets[1], response, &peerError);
            };

        panorama::wire::v1::Request preflightRequest;
        if (!readRequest(
                sockets[1], &preflightRequest, &peerError) ||
            preflightRequest.body_case() !=
                panorama::wire::v1::Request::
                    kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "missing first user configuration preflight");
            return;
        }

        if (dropPreflightResponse) {
            droppedTrackId =
                preflightRequest.header().track_id();
            if (sendLateBeforeRetryRequest) {
                QElapsedTimer delay;
                delay.start();
                while (delay.elapsed() < 175) {
                    const int remaining =
                        175 - static_cast<int>(
                                  delay.elapsed());
                    const int result =
                        ::poll(
                            nullptr, 0,
                            qMax(1, remaining));
                    if (result < 0 && errno != EINTR) {
                        peerError = QStringLiteral(
                            "late response delay failed");
                        return;
                    }
                }
                if (!writeInitialConfiguration(
                        preflightRequest)) {
                    return;
                }
            }
            panorama::wire::v1::Request retryRequest;
            if (!readRequest(
                    sockets[1], &retryRequest, &peerError) ||
                retryRequest.body_case() !=
                    panorama::wire::v1::Request::
                        kUserConfigurationQuery) {
                peerError = QStringLiteral(
                    "missing retried user configuration preflight");
                return;
            }
            retryTrackId = retryRequest.header().track_id();
            if (sendLateDroppedResponse &&
                !sendLateBeforeRetryRequest &&
                !writeInitialConfiguration(preflightRequest)) {
                return;
            }
            if (!writeInitialConfiguration(retryRequest)) {
                return;
            }
        } else if (!writeInitialConfiguration(
                       preflightRequest)) {
            return;
        }

        panorama::wire::v1::Request userRequest;
        if (!readRequest(
                sockets[1], &userRequest, &peerError) ||
            userRequest.body_case() !=
                panorama::wire::v1::Request::
                    kUserConfiguration) {
            peerError = QStringLiteral(
                "missing single user configuration mutation");
            return;
        }
        ++userConfigurationWrites;
        auto acknowledgement = baseResponse(userRequest);
        acknowledgement.mutable_acknowledgement();
        if (!writeResponse(
                sockets[1], acknowledgement, &peerError)) {
            return;
        }

        panorama::wire::v1::Request overlayRequest;
        if (!readRequest(
                sockets[1], &overlayRequest, &peerError) ||
            overlayRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "missing single overlay activation");
            return;
        }
        ++overlayWrites;

        const auto writeAppliedConfiguration =
            [&](const panorama::wire::v1::Request &request) {
                auto response = baseResponse(request);
                *response.mutable_user_configuration() =
                    userRequest.user_configuration();
                return writeResponse(
                    sockets[1], response, &peerError);
            };

        panorama::wire::v1::Request verificationRequest;
        if (!readRequest(
                sockets[1], &verificationRequest,
                &peerError) ||
            verificationRequest.body_case() !=
                panorama::wire::v1::Request::
                    kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "missing first user configuration verification");
            return;
        }

        if (!dropPreflightResponse) {
            droppedTrackId =
                verificationRequest.header().track_id();
            panorama::wire::v1::Request retryRequest;
            if (!readRequest(
                    sockets[1], &retryRequest, &peerError) ||
                retryRequest.body_case() !=
                    panorama::wire::v1::Request::
                        kUserConfigurationQuery) {
                peerError = QStringLiteral(
                    "missing retried user configuration verification");
                return;
            }
            retryTrackId = retryRequest.header().track_id();
            if (!writeAppliedConfiguration(retryRequest)) {
                return;
            }
        } else {
            if (!writeAppliedConfiguration(verificationRequest)) {
                return;
            }
        }
    });

    PrinterProtocol protocol(75);
    const QString devicePath =
        QStringLiteral(
            "/dev/usb/lp-pase-idempotent-query-retry");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);

    PrinterProtocol::PaseApplyConfig config;
    config.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    config.screenMode =
        QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    config.replaceOverlay = true;
    config.overlay.dualMode = true;

    QString error;
    PrinterProtocol::MutationDetails mutation;
    PrinterProtocol::PaseDisplayState appliedState;
    const bool applied =
        protocol.applyPaseConfiguration(
            devicePath, config, &error,
            PrinterProtocol::OperationContext{},
            &mutation, &appliedState);

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(applied, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY(droppedTrackId != 0);
    QVERIFY(retryTrackId != 0);
    QVERIFY(droppedTrackId != retryTrackId);
    QCOMPARE(userConfigurationWrites, 1);
    QCOMPARE(overlayWrites, 1);
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(appliedState.screenMode,
             QStringLiteral("Screen Splitting"));
    QCOMPARE(appliedState.media, config.media);
}

void PrinterProtocolTests::
paseApplyReadOnlyRetryCancellationSendsNoMutation() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));
    const int cancellationFd =
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    QVERIFY(cancellationFd >= 0);

    int queryCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(
                sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::
                    kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "missing user configuration query before retry cancellation");
            return;
        }
        ++queryCount;

        QElapsedTimer delay;
        delay.start();
        while (delay.elapsed() < 150) {
            const int remaining =
                150 - static_cast<int>(delay.elapsed());
            const int result =
                ::poll(nullptr, 0, qMax(1, remaining));
            if (result < 0 && errno != EINTR) {
                peerError = QStringLiteral(
                    "retry cancellation delay failed");
                return;
            }
        }

        const uint64_t cancellationValue = 1;
        if (::write(
                cancellationFd, &cancellationValue,
                sizeof(cancellationValue)) !=
            static_cast<ssize_t>(
                sizeof(cancellationValue))) {
            peerError = QStringLiteral(
                "failed to cancel read-only retry backoff");
            return;
        }

        if (!waitForPeerClosureWithoutPayload(
                sockets[1], 600, &peerError)) {
            return;
        }
    });

    PrinterProtocol protocol(75);
    const QString devicePath =
        QStringLiteral(
            "/dev/usb/lp-pase-idempotent-query-cancel");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);

    PrinterProtocol::PaseApplyConfig config;
    config.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    config.screenMode =
        QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;

    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationFd;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    QElapsedTimer elapsed;
    elapsed.start();
    const bool applied =
        protocol.applyPaseConfiguration(
            devicePath, config, &error, context,
            &mutation);

    peer.join();
    ::close(cancellationFd);
    ::close(sockets[1]);

    QVERIFY(!applied);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(queryCount, 1);
    QCOMPARE(
        mutation.stage,
        QStringLiteral("ReadingConfig"));
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::Cancelled);
    QVERIFY(error.contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY(elapsed.elapsed() < 1000);
}

void PrinterProtocolTests::
paseApplyReadOnlyRetryBudgetIsBounded() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    int queryCount = 0;
    quint64 firstTrackId = 0;
    quint64 secondTrackId = 0;
    QString peerError;
    std::thread peer([&]() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            panorama::wire::v1::Request request;
            if (!readRequest(
                    sockets[1], &request, &peerError) ||
                request.body_case() !=
                    panorama::wire::v1::Request::
                        kUserConfigurationQuery) {
                peerError = QStringLiteral(
                    "missing bounded read-only query attempt %1")
                                .arg(attempt + 1);
                return;
            }
            ++queryCount;
            if (attempt == 0) {
                firstTrackId =
                    request.header().track_id();
            } else {
                secondTrackId =
                    request.header().track_id();
            }
        }

        if (!waitForPeerClosureWithoutPayload(
                sockets[1], 600, &peerError)) {
            return;
        }
    });

    PrinterProtocol protocol(75);
    const QString devicePath =
        QStringLiteral(
            "/dev/usb/lp-pase-idempotent-query-budget");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);

    PrinterProtocol::PaseApplyConfig config;
    config.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    config.screenMode =
        QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;

    QString error;
    PrinterProtocol::MutationDetails mutation;
    QElapsedTimer elapsed;
    elapsed.start();
    const bool applied =
        protocol.applyPaseConfiguration(
            devicePath, config, &error,
            PrinterProtocol::OperationContext{},
            &mutation);

    peer.join();
    ::close(sockets[1]);

    QVERIFY(!applied);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(queryCount, 2);
    QVERIFY(firstTrackId != 0);
    QVERIFY(secondTrackId != 0);
    QVERIFY(firstTrackId != secondTrackId);
    QCOMPARE(
        mutation.stage,
        QStringLiteral("ReadingConfig"));
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::NotStarted);
    QVERIFY(error.contains(
        QStringLiteral("Timed out"), Qt::CaseInsensitive));
    QVERIFY(elapsed.elapsed() < 1500);
}

void PrinterProtocolTests::
paseApplyMismatchedReadOnlyResponseDoesNotRetry() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    int queryCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(
                sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::
                    kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "missing query before mismatched response");
            return;
        }
        ++queryCount;
        auto response = baseResponse(request);
        response.mutable_acknowledgement();
        if (!writeResponse(
                sockets[1], response, &peerError)) {
            return;
        }

        if (!waitForPeerClosureWithoutPayload(
                sockets[1], 600, &peerError)) {
            return;
        }
    });

    PrinterProtocol protocol(75);
    const QString devicePath =
        QStringLiteral(
            "/dev/usb/lp-pase-idempotent-query-invalid");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);

    PrinterProtocol::PaseApplyConfig config;
    config.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    config.screenMode =
        QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;

    QString error;
    PrinterProtocol::MutationDetails mutation;
    const bool applied =
        protocol.applyPaseConfiguration(
            devicePath, config, &error,
            PrinterProtocol::OperationContext{},
            &mutation);

    peer.join();
    ::close(sockets[1]);

    QVERIFY(!applied);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(queryCount, 1);
    QCOMPARE(
        mutation.stage,
        QStringLiteral("ReadingConfig"));
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::NotStarted);
    QVERIFY(error.contains(
        QStringLiteral("does not match"),
        Qt::CaseInsensitive));
}

void PrinterProtocolTests::paseReadbackMismatchIsVerificationFailure() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest,
                         &peerError) ||
            getRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "verification test did not read initial user config");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *initial =
            getResponse.mutable_user_configuration();
        initial->mutable_display_config()
            ->set_backlight_brightness(50);
        initial->mutable_standby_config()
            ->set_media_file("standby.h264");
        auto *initialWork =
            initial->mutable_work_config();
        initialWork->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        initialWork->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        initialWork->set_single_mode_media_file(
            "old.h264");
        if (!writeResponse(sockets[1], getResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request userRequest;
        if (!readRequest(sockets[1], &userRequest,
                         &peerError) ||
            userRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral(
                "verification test did not write user config");
            return;
        }
        auto userResponse = baseResponse(userRequest);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], userResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest,
                         &peerError) ||
            runRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "verification test did not write run config");
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "verification test did not request readback");
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        auto *readback =
            readbackResponse.mutable_user_configuration();
        readback->mutable_display_config()
            ->set_backlight_brightness(50);
        readback->mutable_standby_config()
            ->set_media_file("standby.h264");
        auto *readbackWork =
            readback->mutable_work_config();
        readbackWork->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        readbackWork->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        readbackWork->set_single_mode_media_file(
            "old.h264");
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-readback-mismatch");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);
    PrinterProtocol::PaseApplyConfig config;
    config.media = {QStringLiteral("left.h264"),
                    QStringLiteral("right.h264")};
    config.screenMode =
        QStringLiteral("Screen Splitting");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    PrinterProtocol::PaseDisplayState appliedState;
    appliedState.screenMode =
        QStringLiteral("sentinel");
    const bool applied = protocol.applyPaseConfiguration(
        devicePath, config, &error,
        PrinterProtocol::OperationContext{}, &mutation,
        &appliedState);
    peer.join();
    ::close(sockets[1]);

    QVERIFY(!applied);
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::VerificationFailed);
    QCOMPARE(mutation.stage,
             QStringLiteral("VerifyingConfig"));
    QVERIFY(error.contains(
        QStringLiteral("screen mode"),
        Qt::CaseInsensitive));
    QCOMPARE(appliedState.screenMode,
             QStringLiteral("Full Screen"));
    QCOMPARE(
        appliedState.media,
        QStringList{QStringLiteral("old.h264")});
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
runConfigRejectionIsVerificationFailureAndKeepsTransport() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    panorama::wire::v1::Request capturedUserConfig;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(
                sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "RunConfig rejection test did not read user config");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *initial =
            getResponse.mutable_user_configuration();
        initial->mutable_display_config()
            ->set_backlight_brightness(50);
        initial->mutable_standby_config()
            ->set_media_file("standby.h264");
        auto *work = initial->mutable_work_config();
        work->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        work->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        work->set_single_mode_media_file("old.h264");
        if (!writeResponse(
                sockets[1], getResponse, &peerError)) {
            return;
        }

        if (!readRequest(
                sockets[1], &capturedUserConfig,
                &peerError) ||
            capturedUserConfig.body_case() !=
                panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral(
                "RunConfig rejection test did not write user config");
            return;
        }
        auto userResponse =
            baseResponse(capturedUserConfig);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(
                sockets[1], userResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(
                sockets[1], &runRequest, &peerError) ||
            runRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "RunConfig rejection test did not receive activation");
            return;
        }
        auto runResponse = baseResponse(runRequest);
        runResponse.mutable_error()->set_code(
            panorama::wire::v1::ProtocolError::FAILURE);
        runResponse.mutable_error()->set_why(
            "overlay rejected");
        if (!writeResponse(
                sockets[1], runResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(
                sockets[1], &readbackRequest, &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "RunConfig rejection test did not read back user config");
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        *readbackResponse.mutable_user_configuration() =
            capturedUserConfig.user_configuration();
        if (!writeResponse(
                sockets[1], readbackResponse,
                &peerError)) {
            return;
        }

        panorama::wire::v1::Request fileListRequest;
        if (!readRequest(
                sockets[1], &fileListRequest, &peerError) ||
            fileListRequest.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral(
                "RunConfig rejection closed a healthy transport");
            return;
        }
        auto fileListResponse =
            baseResponse(fileListRequest);
        fileListResponse.mutable_media_catalog();
        writeResponse(
            sockets[1], fileListResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString endpoint =
        QStringLiteral(
            "/dev/usb/lp-pase-runconfig-rejected");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], endpoint);
    PrinterProtocol::PaseApplyConfig config;
    config.display.brightnessPresent = true;
    config.display.brightness = 77;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    PrinterProtocol::PaseDisplayState appliedState;
    QVERIFY(!protocol.applyPaseConfiguration(
        endpoint, config, &error,
        PrinterProtocol::OperationContext{}, &mutation,
        &appliedState));
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::VerificationFailed);
    QCOMPARE(mutation.stage,
             QStringLiteral("VerifyingConfig"));
    QCOMPARE(appliedState.brightness, 77);
    QVERIFY(error.contains(
        QStringLiteral("overlay activation"),
        Qt::CaseInsensitive));

    const PrinterProtocol::MediaListResult fileList =
        protocol.readMediaList(
            endpoint,
            PrinterProtocol::OperationContext{});
    QVERIFY2(fileList.success,
             qPrintable(fileList.error));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
verificationFailureKeepsHealthySessionActive() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest,
                         &peerError)) {
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *initial =
            getResponse.mutable_user_configuration();
        initial->mutable_display_config();
        auto *initialWork =
            initial->mutable_work_config();
        initialWork->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        initialWork->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        initialWork->set_single_mode_media_file(
            "old.h264");
        if (!writeResponse(sockets[1], getResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request userRequest;
        if (!readRequest(sockets[1], &userRequest,
                         &peerError)) {
            return;
        }
        auto userResponse = baseResponse(userRequest);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], userResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest,
                         &peerError)) {
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError)) {
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        auto *readback =
            readbackResponse.mutable_user_configuration();
        readback->mutable_display_config();
        auto *readbackWork =
            readback->mutable_work_config();
        readbackWork->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        readbackWork->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        readbackWork->set_single_mode_media_file(
            "old.h264");
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    constexpr quint64 generation = 44;
    const QString endpoint =
        QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"),
        generation);
    worker.adoptPrinterFileDescriptorForTesting(
        sockets[0], endpoint);
    worker.printerSessionState_ =
        DeviceWorker::PrinterSessionState::Active;
    worker.printerRecoveryTimer_->stop();

    TryxRuntimeApplyRequest request;
    request.media = {QStringLiteral("left.h264"),
                     QStringLiteral("right.h264")};
    request.screenMode =
        QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    QSignalSpy applySpy(
        &worker, &DeviceWorker::printerApplyFinished);
    QSignalSpy displaySpy(
        &worker, &DeviceWorker::printerDisplayStateReady);
    worker.applyPrinterMedia(
        endpoint, QString(), request, false,
        QStringLiteral(
            "44444444-4444-4444-8444-444444444444"),
        generation);

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(displaySpy.count(), 1);
    const PrinterProtocol::PaseDisplayState actualState =
        qvariant_cast<PrinterProtocol::PaseDisplayState>(
            displaySpy.first().at(0));
    QCOMPARE(actualState.screenMode,
             QStringLiteral("Full Screen"));
    QCOMPARE(applySpy.first().at(2).toBool(), false);
    QCOMPARE(
        qvariant_cast<PrinterProtocol::MutationOutcome>(
            applySpy.first().at(4)),
        PrinterProtocol::MutationOutcome::VerificationFailed);
    QCOMPARE(
        worker.printerSessionState_,
        DeviceWorker::PrinterSessionState::Active);
    QVERIFY(!worker.printerRecoveryTimer_->isActive());
}

void PrinterProtocolTests::
applyFailureResultPrecedesSessionLoss() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    int queryCount = 0;
    QString peerError;
    std::thread peer([&]() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            panorama::wire::v1::Request request;
            if (!readRequest(
                    sockets[1], &request, &peerError) ||
                request.body_case() !=
                    panorama::wire::v1::Request::
                        kUserConfigurationQuery) {
                peerError = QStringLiteral(
                    "missing read-only apply query %1 before session loss")
                                .arg(attempt + 1);
                return;
            }
            ++queryCount;
        }
    });

    constexpr quint64 generation = 45;
    const QString endpoint =
        QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"),
        generation);
    worker.printerProtocol_ =
        std::make_unique<PrinterProtocol>(75);
    worker.adoptPrinterFileDescriptorForTesting(
        sockets[0], endpoint);
    worker.printerSessionState_ =
        DeviceWorker::PrinterSessionState::Active;

    QStringList terminalEvents;
    connect(
        &worker, &DeviceWorker::printerApplyFinished,
        &worker,
        [&terminalEvents](
            const QString &, const QString &, bool, bool,
            PrinterProtocol::MutationOutcome,
            const QString &, quint64) {
            terminalEvents.append(
                QStringLiteral("apply-finished"));
        });
    connect(
        &worker, &DeviceWorker::printerSessionLost,
        &worker,
        [&terminalEvents](quint64) {
            terminalEvents.append(
                QStringLiteral("session-lost"));
        });
    QSignalSpy applySpy(
        &worker, &DeviceWorker::printerApplyFinished);
    QSignalSpy lostSpy(
        &worker, &DeviceWorker::printerSessionLost);

    TryxRuntimeApplyRequest request;
    request.media = {
        QStringLiteral("left.h264"),
        QStringLiteral("right.h264")};
    request.screenMode =
        QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    worker.applyPrinterMedia(
        endpoint, QString(), request, false,
        QStringLiteral(
            "45454545-4545-4545-8545-454545454545"),
        generation);

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(queryCount, 2);
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(lostSpy.count(), 1);
    QCOMPARE(
        qvariant_cast<PrinterProtocol::MutationOutcome>(
            applySpy.first().at(4)),
        PrinterProtocol::MutationOutcome::NotStarted);
    QCOMPARE(
        terminalEvents,
        QStringList({
            QStringLiteral("apply-finished"),
            QStringLiteral("session-lost")}));
    QCOMPARE(
        worker.printerSessionState_,
        DeviceWorker::PrinterSessionState::Lost);
}

void PrinterProtocolTests::lateRunConfigDummyDoesNotBreakReadback() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest,
                         &peerError)) {
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *initial =
            getResponse.mutable_user_configuration();
        initial->mutable_display_config();
        initial->mutable_standby_config();
        initial->mutable_work_config()
            ->set_single_mode_media_file("old.h264");
        if (!writeResponse(sockets[1], getResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request userRequest;
        if (!readRequest(sockets[1], &userRequest,
                         &peerError)) {
            return;
        }
        auto userResponse = baseResponse(userRequest);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], userResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest,
                         &peerError)) {
            return;
        }
        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "late Dummy test did not receive readback request");
            return;
        }

        auto lateDummy = baseResponse(runRequest);
        lateDummy.mutable_acknowledgement();
        if (!writeResponse(sockets[1], lateDummy,
                           &peerError)) {
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        *readbackResponse.mutable_user_configuration() =
            userRequest.user_configuration();
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-late-dummy");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], devicePath);
    PrinterProtocol::PaseApplyConfig config;
    config.media = {QStringLiteral("new.h264")};
    config.screenMode = QStringLiteral("Full Screen");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    PrinterProtocol::PaseDisplayState appliedState;
    const bool applied = protocol.applyPaseConfiguration(
        devicePath, config, &error,
        PrinterProtocol::OperationContext{}, &mutation,
        &appliedState);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(applied, qPrintable(error));
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(appliedState.media, config.media);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::paseDisplayMutationMatrix_data() {
    QTest::addColumn<bool>("mirrorMode");
    QTest::addColumn<bool>("waterfallMode");
    QTest::addColumn<quint32>("expectedUiRotation");
    QTest::addColumn<quint32>("expectedMediaRotation");

    QTest::newRow("normal")
        << false << false << 0U << 0U;
    QTest::newRow("mirror")
        << true << false << 0U << 180U;
    QTest::newRow("waterfall")
        << false << true << 90U << 0U;
    QTest::newRow("mirror-waterfall")
        << true << true << 90U << 180U;
}

void PrinterProtocolTests::paseDisplayMutationMatrix() {
    QFETCH(bool, mirrorMode);
    QFETCH(bool, waterfallMode);
    QFETCH(quint32, expectedUiRotation);
    QFETCH(quint32, expectedMediaRotation);

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    panorama::wire::v1::Request capturedUserConfig;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "display mutation did not read user config");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *userConfig = getResponse.mutable_user_configuration();
        auto *display = userConfig->mutable_display_config();
        display->set_backlight_enable(true);
        display->set_backlight_brightness(21);
        display->set_mirror(true);
        display->set_ui_rotation(270);
        display->set_media_rotation(90);
        auto *standby = userConfig->mutable_standby_config();
        standby->set_enable(true);
        standby->set_media_file("keep-standby.h264");
        auto *work = userConfig->mutable_work_config();
        work->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
        work->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_ALL);
        work->set_single_mode_media_file("keep-media.h264");
        userConfig->GetReflection()
            ->MutableUnknownFields(userConfig)
            ->AddVarint(199, 42);
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        if (!readRequest(sockets[1], &capturedUserConfig,
                         &peerError) ||
            capturedUserConfig.body_case() !=
                panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral(
                "display mutation did not send user config");
            return;
        }
        auto userResponse = baseResponse(capturedUserConfig);
        userResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], userResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest, &peerError) ||
            runRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "display mutation did not activate run config");
            return;
        }
        auto runResponse = baseResponse(runRequest);
        runResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], runResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "display mutation did not verify user config");
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        *readbackResponse.mutable_user_configuration() =
            capturedUserConfig.user_configuration();
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-display-mutation");
    protocol.adoptFileDescriptorForTesting(sockets[0], devicePath);
    PrinterProtocol::PaseApplyConfig config;
    config.display.brightnessPresent = true;
    config.display.brightness = 68;
    config.display.backlightPresent = true;
    config.display.backlightEnabled = false;
    config.display.orientationPresent = true;
    config.display.mirrorMode = mirrorMode;
    config.display.waterfallMode = waterfallMode;
    QString error;
    PrinterProtocol::PaseDisplayState appliedState;
    const bool applied = protocol.applyPaseConfiguration(
        devicePath, config, &error,
        PrinterProtocol::OperationContext{}, nullptr,
        &appliedState);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(applied, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    const auto &userConfig = capturedUserConfig.user_configuration();
    QCOMPARE(userConfig.display_config().backlight_enable(), false);
    QCOMPARE(userConfig.display_config().backlight_brightness(), 68U);
    QCOMPARE(userConfig.display_config().mirror(), false);
    QCOMPARE(userConfig.display_config().ui_rotation(),
             expectedUiRotation);
    QCOMPARE(userConfig.display_config().media_rotation(),
             expectedMediaRotation);
    QCOMPARE(userConfig.standby_config().enable(), true);
    QCOMPARE(QString::fromStdString(
                 userConfig.standby_config().media_file()),
             QStringLiteral("keep-standby.h264"));
    QCOMPARE(QString::fromStdString(
                 userConfig.work_config().single_mode_media_file()),
             QStringLiteral("keep-media.h264"));
    QVERIFY(hasUnknownField(
        userConfig.GetReflection()->GetUnknownFields(userConfig), 199));
    QCOMPARE(appliedState.backlightEnabled, false);
    QCOMPARE(appliedState.brightness, 68);
    QCOMPARE(appliedState.standbyEnabled, true);
    QCOMPARE(appliedState.standbyMedia,
             QStringLiteral("keep-standby.h264"));
    QCOMPARE(appliedState.mirrorMode, mirrorMode);
    QCOMPARE(appliedState.waterfallMode, waterfallMode);
    QCOMPARE(appliedState.playMode, QStringLiteral("Loop"));
    QCOMPARE(appliedState.media,
             QStringList{QStringLiteral("keep-media.h264")});
}

void PrinterProtocolTests::readPaseDisplayStateDecodesDualConfiguration() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "display-state read did not request user config");
            return;
        }
        auto response = baseResponse(request);
        auto *userConfig = response.mutable_user_configuration();
        auto *display = userConfig->mutable_display_config();
        display->set_backlight_enable(true);
        display->set_backlight_brightness(83);
        display->set_ui_rotation(90);
        display->set_media_rotation(180);
        auto *standby = userConfig->mutable_standby_config();
        standby->set_enable(false);
        standby->set_media_file("standby.h264");
        auto *work = userConfig->mutable_work_config();
        work->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_DUAL);
        work->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
        work->set_dual_mode_left_media_file("left.h264");
        work->set_dual_mode_right_media_file("right.h264");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-display-state");
    protocol.adoptFileDescriptorForTesting(sockets[0], devicePath);
    const PrinterProtocol::PaseDisplayStateResult result =
        protocol.readPaseDisplayState(
            devicePath, PrinterProtocol::OperationContext{});
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(result.state.backlightEnabled, true);
    QCOMPARE(result.state.brightness, 83);
    QCOMPARE(result.state.standbyEnabled, false);
    QCOMPARE(result.state.standbyMedia,
             QStringLiteral("standby.h264"));
    QCOMPARE(result.state.mirrorMode, true);
    QCOMPARE(result.state.waterfallMode, true);
    QCOMPARE(result.state.screenMode,
             QStringLiteral("Screen Splitting"));
    QCOMPARE(result.state.playMode, QStringLiteral("Single"));
    QCOMPARE(result.state.media,
             QStringList({QStringLiteral("left.h264"),
                          QStringLiteral("right.h264")}));
}

void PrinterProtocolTests::standalonePaseMetricsConfigurationIsAcknowledged() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    PrinterProtocol protocol;
    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-metrics-config");
    protocol.adoptFileDescriptorForTesting(sockets[0], devicePath);
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        if (!readRequest(sockets[1], &captured, &peerError)) {
            return;
        }
        auto response = baseResponse(captured);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {QStringLiteral("CPU Power")};
    overlay.left.alignment = QStringLiteral("Right");
    overlay.left.textColor = 0x000000U;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    const bool configured = protocol.configurePaseOverlay(
        devicePath, overlay, &error, PrinterProtocol::OperationContext{},
        &mutation);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(configured, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(mutation.stage, QStringLiteral("ActivatingMetricsLayout"));
    QCOMPARE(captured.body_case(),
             panorama::wire::v1::Request::kOverlayLayout);
    QCOMPARE(captured.overlay_layout().label_groups_size(), 1);
    const auto &group = captured.overlay_layout().label_groups(0);
    QCOMPARE(group.group_id(), 103U);
    QCOMPARE(group.text_align(), panorama::wire::v1::OverlayGroup::ALIGN_RIGHT);
    QCOMPARE(group.labels_size(), 3);
    QCOMPARE(group.labels(0).label_id(), 110U);
    QCOMPARE(group.labels(1).label_id(), 111U);
    QCOMPARE(group.labels(2).label_id(), 112U);
    QCOMPARE(group.labels(0).text_color(), 0x000000U);
}

void PrinterProtocolTests::displayKeepalivePreservesPaseOverlayValues() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    PrinterProtocol protocol;
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("/dev/usb/lp-pase-keepalive"));
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        if (!readRequest(sockets[1], &captured, &peerError)) {
            return;
        }
        auto response = baseResponse(captured);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {QStringLiteral("CPU Temperature")};
    overlay.left.initialLabels = {
        QStringLiteral("CPU Temperature")};
    overlay.left.initialValues = {QStringLiteral("56")};
    overlay.left.initialUnits = {QStringLiteral("°C")};
    QString error;
    QCOMPARE(protocol.sendDisplayKeepalive(
                 QStringLiteral("/dev/usb/lp-pase-keepalive"), &error,
                 PrinterProtocol::OperationContext{}, &overlay),
             PrinterProtocol::KeepaliveOutcome::Sent);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(captured.body_case(),
             panorama::wire::v1::Request::kOverlayLayout);
    QCOMPARE(captured.overlay_layout().label_groups_size(), 1);
    const auto &group = captured.overlay_layout().label_groups(0);
    QCOMPARE(group.labels_size(), 3);
    QCOMPARE(QString::fromStdString(group.labels(1).text()),
             QStringLiteral("56"));
    QCOMPARE(QString::fromStdString(group.labels(2).text()),
             QStringLiteral("°C"));
}

void PrinterProtocolTests::paseMetricBatchMatchesHeaderlessWireFrame() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    PrinterProtocol protocol;
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("/dev/usb/lp-pase-batch"));
    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        readRequest(sockets[1], &captured, &peerError);
    });

    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {QStringLiteral("CPU Temperature")};
    QString error;
    const bool sent = protocol.sendPaseMetricBatch(
        QStringLiteral("/dev/usb/lp-pase-batch"), overlay,
        {QStringLiteral("CPU Temperature")}, {QStringLiteral("56")},
        {QStringLiteral("°C")}, &error,
        PrinterProtocol::OperationContext{});
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(sent, qPrintable(error));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY(!captured.has_header());
    QCOMPARE(captured.body_case(),
             panorama::wire::v1::Request::kMetricBatch);
    std::string serialized;
    QVERIFY(captured.SerializeToString(&serialized));
    const QByteArray actual(serialized.data(),
                            static_cast<qsizetype>(serialized.size()));
    const QByteArray expected = QByteArray::fromHex(
        "e212190a0a086412060866120235360a0b0864120708671203c2b043");
    QCOMPARE(actual, expected);
}

void PrinterProtocolTests::metricBatchExplicitErrorIsRejected() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::
                    kMetricBatch) {
            peerError = QStringLiteral(
                "metric error test did not receive a batch update");
            return;
        }
        panorama::wire::v1::Response response;
        response.mutable_error()->set_code(
            panorama::wire::v1::ProtocolError::FAILURE);
        response.mutable_error()->set_why(
            "label group is unavailable");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol;
    const QString endpoint =
        QStringLiteral("/dev/usb/lp-pase-batch-error");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], endpoint);
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    QString error;
    QVERIFY(!protocol.sendPaseMetricBatch(
        endpoint, overlay,
        {QStringLiteral("CPU Temperature")},
        {QStringLiteral("56")},
        {QStringLiteral("°C")}, &error,
        PrinterProtocol::OperationContext{}));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY(error.contains(
        QStringLiteral("label group"),
        Qt::CaseInsensitive));
}

void PrinterProtocolTests::metricBatchResponsesAreDrainedBeforeTrackedRequest() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int responseReadyFd = eventfd(0, EFD_CLOEXEC);
    QVERIFY(responseReadyFd >= 0);

    constexpr int kMetricResponseCount = 40;
    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        for (int index = 0; index < kMetricResponseCount; ++index) {
            QByteArray payload;
            panorama::wire::v1::Request request;
            if (!readFrameFd(sockets[1], &payload, &peerError,
                             kPeerTimeoutMs, &requestBuffer) ||
                !request.ParseFromArray(payload.constData(),
                                        static_cast<int>(payload.size())) ||
                request.body_case() !=
                    panorama::wire::v1::Request::kMetricBatch) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "expected metric batch request %1").arg(index);
                }
                return;
            }
            panorama::wire::v1::Response response;
            response.mutable_acknowledgement();
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }
        const uint64_t ready = 1;
        if (::write(responseReadyFd, &ready, sizeof(ready)) !=
            static_cast<ssize_t>(sizeof(ready))) {
            peerError = QStringLiteral(
                "failed to signal queued metric response burst");
            return;
        }

        QByteArray trackedPayload;
        panorama::wire::v1::Request request;
        if (!readFrameFd(sockets[1], &trackedPayload, &peerError,
                         kPeerTimeoutMs, &requestBuffer) ||
            !request.ParseFromArray(
                trackedPayload.constData(),
                static_cast<int>(trackedPayload.size())) ||
            request.body_case() != panorama::wire::v1::Request::kPing) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "tracked request did not follow metric response burst");
            }
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("queue-drained");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {QStringLiteral("CPU Temperature")};
    QString mainError;
    for (int index = 0; index < kMetricResponseCount; ++index) {
        if (!protocol.sendPaseMetricBatch(
                QStringLiteral("test-endpoint"), overlay,
                {QStringLiteral("CPU Temperature")},
                {QString::number(50 + index % 10)}, {QStringLiteral("°C")},
                &mainError, PrinterProtocol::OperationContext{})) {
            break;
        }
    }
    if (mainError.isEmpty()) {
        pollfd descriptor{};
        descriptor.fd = responseReadyFd;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, kPeerTimeoutMs) != 1) {
            mainError = QStringLiteral(
                "timed out waiting for queued metric response burst");
        } else {
            uint64_t ready = 0;
            if (::read(responseReadyFd, &ready, sizeof(ready)) !=
                static_cast<ssize_t>(sizeof(ready)) ||
                ready == 0) {
                mainError = QStringLiteral(
                    "failed to consume queued metric response signal");
            }
        }
    }

    QString payload;
    QString trackedError;
    bool trackedSuccess = false;
    if (mainError.isEmpty()) {
        trackedSuccess = protocol.trackedPingForTesting(
            QStringLiteral("test-endpoint"), &payload, &trackedError,
            PrinterProtocol::OperationContext{});
    } else {
        protocol.close();
    }

    peer.join();
    ::close(responseReadyFd);
    ::close(sockets[1]);

    QVERIFY2(mainError.isEmpty(), qPrintable(mainError));
    QVERIFY2(trackedSuccess, qPrintable(trackedError));
    QCOMPARE(payload, QStringLiteral("queue-drained"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::daemonMetricsBatchRunsWithoutGui() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    const QString devicePath =
        QStringLiteral("/dev/usb/lp-pase-daemon-metrics");
    constexpr quint64 generation = 17;
    DeviceWorker worker;
    worker.configurePrinterDevice(devicePath, QStringLiteral("PASE-17"),
                                  generation);
    worker.updatePrinterGenerationGate(generation, true);
    worker.adoptPrinterFileDescriptorForTesting(sockets[0], devicePath);
    worker.printerSessionState_ = DeviceWorker::PrinterSessionState::Active;
    worker.printerOverlayConfig_.left.metrics = {
        QStringLiteral("Date&Time")};

    panorama::wire::v1::Request captured;
    QString peerError;
    std::thread peer([&]() {
        readRequest(sockets[1], &captured, &peerError);
    });
    worker.printerKeepaliveTimer_->start(5000);
    QSignalSpy sentSpy(&worker, &DeviceWorker::printerSysinfoSent);
    worker.sendPrinterMetrics();
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(sentSpy.count(), 1);
    QCOMPARE(sentSpy.first().at(0).toULongLong(), generation);
    QVERIFY(!captured.has_header());
    QCOMPARE(captured.body_case(),
             panorama::wire::v1::Request::kMetricBatch);
    QCOMPARE(captured.metric_batch().label_groups_size(), 2);
    QCOMPARE(captured.metric_batch().label_groups(0).group_id(),
             110U);
    QCOMPARE(captured.metric_batch()
                 .label_groups(0)
                 .label_texts(0)
                 .label_id(),
             131U);
    QCOMPARE(captured.metric_batch()
                 .label_groups(1)
                 .label_texts(0)
                 .label_id(),
             132U);
    QCOMPARE(worker.printerKeepaliveTimer_->interval(), 5000);
    worker.printerKeepaliveTimer_->stop();
}

void PrinterProtocolTests::foregroundOperationPausesMetricsAndKeepalive() {
    DeviceWorker worker;
    constexpr quint64 generation = 23;
    worker.configurePrinterDevice(
        QStringLiteral("/dev/usb/lp-pase-pause"),
        QStringLiteral("PASE-23"), generation);
    worker.updatePrinterGenerationGate(generation, true);
    worker.printerSessionState_ = DeviceWorker::PrinterSessionState::Active;
    worker.printerOverlayConfig_.left.metrics = {
        QStringLiteral("Date&Time")};
    worker.printerKeepaliveTimer_->start(10000);
    worker.printerMetricsTimer_->start(1000);

    const QString operationId =
        QStringLiteral("17171717-1717-4717-8717-171717171717");
    worker.beginPrinterForegroundOperation(operationId, generation);
    QCOMPARE(worker.foregroundPrinterOperationId_, operationId);
    QVERIFY(!worker.printerKeepaliveTimer_->isActive());
    QVERIFY(!worker.printerMetricsTimer_->isActive());

    worker.endPrinterForegroundOperation(
        QStringLiteral("18181818-1818-4818-8818-181818181818"),
        generation);
    QCOMPARE(worker.foregroundPrinterOperationId_, operationId);
    QVERIFY(!worker.printerMetricsTimer_->isActive());

    worker.endPrinterForegroundOperation(operationId, generation);
    QVERIFY(worker.foregroundPrinterOperationId_.isEmpty());
    QVERIFY(worker.printerKeepaliveTimer_->isActive());
    QVERIFY(worker.printerMetricsTimer_->isActive());
    worker.printerKeepaliveTimer_->stop();
    worker.printerMetricsTimer_->stop();
}

void PrinterProtocolTests::raplPowerUsesMonotonicBoundedInterval() {
    double powerWatts = 0.0;
    QVERIFY(SystemMonitor::calculateRaplPowerWatts(
        1000000, 2000000, 0, 1000, &powerWatts));
    QCOMPARE(powerWatts, 1.0);

    QVERIFY(!SystemMonitor::calculateRaplPowerWatts(
        2000000, 3000000, 0, 6000, &powerWatts));
    QVERIFY(!SystemMonitor::calculateRaplPowerWatts(
        2000000, 3000000, 0, 0, &powerWatts));

    QVERIFY(SystemMonitor::calculateRaplPowerWatts(
        9500000, 500000, 10000000, 1000, &powerWatts));
    QCOMPARE(powerWatts, 1.0);

    SystemMonitor monitor;
    bool memoryFrequencyAvailable = true;
    QCOMPARE(monitor.readMemoryFrequency(&memoryFrequencyAvailable), 0.0);
    QVERIFY(!memoryFrequencyAvailable);
}

void PrinterProtocolTests::hardwareBadgeModelsResolve() {
    QCOMPARE(
        SystemMonitor::cpuModelNameFromContents(
            QByteArrayLiteral(
                "processor : 0\n"
                "Hardware : AMD Ryzen AI 9 365\n")),
        QStringLiteral("AMD Ryzen AI 9 365"));
    const QString cpuModel =
        SystemMonitor::cpuModelName().trimmed();
    QVERIFY(!cpuModel.isEmpty());
    QVERIFY(cpuModel != QStringLiteral("CPU"));

    QTemporaryFile idsFile;
    QVERIFY(idsFile.open());
    const QByteArray idsContents =
        QByteArrayLiteral(
            "# device, revision, marketing name\n"
            "744C,\tC8,\tAMD Radeon RX 7900 XTX\n");
    QCOMPARE(idsFile.write(idsContents),
             static_cast<qint64>(idsContents.size()));
    QVERIFY(idsFile.flush());
    QCOMPARE(
        SystemMonitor::readAmdGpuMarketingNameFromIdsFile(
            idsFile.fileName(), QStringLiteral("0x744c"),
            QStringLiteral("0xc8")),
        QStringLiteral("AMD Radeon RX 7900 XTX"));

    QTemporaryDir drmFixture;
    QVERIFY(drmFixture.isValid());
    const QString nvidiaDevice =
        QDir(drmFixture.path()).filePath(
            QStringLiteral("card0/device"));
    QVERIFY(writeTextFile(
        QDir(nvidiaDevice).filePath(
            QStringLiteral("product_name")),
        QByteArrayLiteral("NVIDIA GeForce RTX 5090\n")));
    QVERIFY(writeTextFile(
        QDir(nvidiaDevice).filePath(
            QStringLiteral("vendor")),
        QByteArrayLiteral("0x10de\n")));
    QVERIFY(writeTextFile(
        QDir(nvidiaDevice).filePath(
            QStringLiteral("boot_vga")),
        QByteArrayLiteral("1\n")));
    SystemMonitor fixtureMonitor;
    QCOMPARE(
        fixtureMonitor.primaryGpuModelNameFromDrmRoot(
            drmFixture.path()),
        QStringLiteral("NVIDIA GeForce RTX 5090"));

    bool amdMetricsCardPresent = false;
    QDir drmDirectory(QStringLiteral("/sys/class/drm"));
    for (const QString &entry : drmDirectory.entryList(
             QStringList{QStringLiteral("card[0-9]*")},
             QDir::Dirs)) {
        const QString devicePath =
            drmDirectory.filePath(entry) +
            QStringLiteral("/device");
        if (QFile::exists(
                devicePath +
                QStringLiteral("/gpu_busy_percent"))) {
            amdMetricsCardPresent = true;
            break;
        }
    }
    if (amdMetricsCardPresent) {
        SystemMonitor monitor;
        const QString gpuModel =
            monitor.primaryGpuModelName().trimmed();
        QVERIFY(!gpuModel.isEmpty());
        QVERIFY(!QRegularExpression(
                     QStringLiteral("^GPU\\s*\\d*$"),
                     QRegularExpression::CaseInsensitiveOption)
                     .match(gpuModel)
                     .hasMatch());
    }
}

void PrinterProtocolTests::metricsConfigurationValidatesPersistsAndDisables() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestPrinterConfigureMetrics,
        manager->worker_, &DeviceWorker::configurePrinterMetrics);
    QObject::disconnect(
        manager.get(), &DeviceManager::requestPrinterApplyMedia,
        manager->worker_, &DeviceWorker::applyPrinterMedia);
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    QCOMPARE(manager->printerDeviceSerial_, QStringLiteral("1-1"));
    QCOMPARE(manager->metricsCapabilities().size(), 11);
    QVERIFY(manager->metricsCapabilities().contains(
        QStringLiteral("CPU Power")));
    QVERIFY(manager->metricsCapabilities().contains(
        QStringLiteral("Memory Frequency")));
    QVERIFY(manager->metricsCapabilities().contains(
        QStringLiteral("Date&Time")));

    TryxRuntimeMetricsConfigRequest duplicateRequest;
    duplicateRequest.enabled = true;
    duplicateRequest.metrics = {
        QStringLiteral("CPU Usage"), QStringLiteral("CPU Usage")};
    const QString duplicateId =
        QStringLiteral("19191919-1919-4919-8919-191919191919");
    QCOMPARE(manager->queueMetricsConfigOperation(duplicateId,
                                                  duplicateRequest),
             duplicateId);
    QCOMPARE(manager->operationInfo(duplicateId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(duplicateId).errorCategory,
             QStringLiteral("UnsupportedConfiguration"));

    TryxRuntimeMetricsConfigRequest enableRequest;
    enableRequest.enabled = true;
    enableRequest.metrics = {
        QStringLiteral("CPU Power"), QStringLiteral("GPU Temperature"),
        QStringLiteral("Date&Time")};
    enableRequest.alignment = QStringLiteral("Center");
    enableRequest.textColor = 0xFF0000U;
    QSignalSpy configureSpy(
        manager.get(), &DeviceManager::requestPrinterConfigureMetrics);
    const QString enableId =
        QStringLiteral("20202020-2020-4020-8020-202020202020");
    QCOMPARE(manager->queueMetricsConfigOperation(enableId, enableRequest),
             enableId);
    QCOMPARE(configureSpy.count(), 1);
    QCOMPARE(manager->operationInfo(enableId).state,
             QStringLiteral("Preflight"));
    manager->worker_->printerMetricsConfigured(
        enableId, true, PrinterProtocol::MutationOutcome::Succeeded,
        QString(), manager->printerGeneration_);

    QCOMPARE(manager->operationInfo(enableId).state,
             QStringLiteral("Succeeded"));
    const TryxRuntimeMetricsState enabledState = manager->metricsState();
    QVERIFY(enabledState.enabled);
    QVERIFY(enabledState.samplingActive);
    QCOMPARE(enabledState.deviceSerial, QStringLiteral("1-1"));
    QCOMPARE(enabledState.metrics, enableRequest.metrics);
    QCOMPARE(enabledState.alignment, enableRequest.alignment);
    QCOMPARE(enabledState.textColor, enableRequest.textColor);
    QVERIFY(enabledState.diagnostic.isEmpty());
    QVERIFY(QFileInfo::exists(manager->paseMetricsConfigPath()));

    std::unique_ptr<DeviceManager> restored(
        DeviceManager::createForTesting(sysRoot, devRoot));
    restored->loadPaseMetricsConfig();
    const PrinterProtocol::PaseOverlayConfig restoredOverlay =
        restored->persistedPaseOverlayForDevice(QStringLiteral("1-1"));
    QCOMPARE(restoredOverlay.left.metrics, enableRequest.metrics);
    QCOMPARE(restoredOverlay.left.alignment, enableRequest.alignment);
    QCOMPARE(restoredOverlay.left.textColor, enableRequest.textColor);
    QVERIFY(restored->persistedPaseOverlayForDevice(
                          QStringLiteral("different-device"))
                .left.metrics
                .isEmpty());

    PrinterProtocol::PaseOverlayConfig dualOverlay;
    dualOverlay.dualMode = true;
    dualOverlay.waterfallMode = true;
    dualOverlay.left.metrics = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Power")};
    dualOverlay.left.badges = {
        QStringLiteral("CPU Badge")};
    dualOverlay.left.alignment =
        QStringLiteral("Right");
    dualOverlay.left.textColor = 0xFF0000U;
    dualOverlay.left.verticalPlacement =
        QStringLiteral("Bottom");
    dualOverlay.right.metrics = {
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Power")};
    dualOverlay.right.badges = {
        QStringLiteral("GPU Badge")};
    dualOverlay.right.alignment =
        QStringLiteral("Left");
    dualOverlay.right.textColor = 0x00FF00U;
    dualOverlay.right.verticalPlacement =
        QStringLiteral("Top");
    QString dualPersistError;
    QVERIFY2(manager->persistPaseMetricsConfiguration(
                 dualOverlay, true, &dualPersistError),
             qPrintable(dualPersistError));
    QFile dualConfigFile(
        manager->paseMetricsConfigPath());
    QVERIFY(dualConfigFile.open(QIODevice::ReadOnly));
    const QJsonObject dualRoot =
        QJsonDocument::fromJson(
            dualConfigFile.readAll()).object();
    QCOMPARE(dualRoot.value(
                 QStringLiteral("version")).toInt(),
             2);
    QCOMPARE(dualRoot.value(
                 QStringLiteral("dualMode")).toBool(),
             true);
    QCOMPARE(dualRoot.value(
                 QStringLiteral("waterfallMode")).toBool(),
             true);

    std::unique_ptr<DeviceManager> dualRestored(
        DeviceManager::createForTesting(sysRoot, devRoot));
    dualRestored->loadPaseMetricsConfig();
    const PrinterProtocol::PaseOverlayConfig
        restoredDual =
            dualRestored->persistedPaseOverlayForDevice(
                QStringLiteral("1-1"));
    QCOMPARE(restoredDual.dualMode, true);
    QCOMPARE(restoredDual.waterfallMode, true);
    QCOMPARE(restoredDual.left.metrics,
             dualOverlay.left.metrics);
    QCOMPARE(restoredDual.left.badges,
             dualOverlay.left.badges);
    QCOMPARE(restoredDual.left.alignment,
             dualOverlay.left.alignment);
    QCOMPARE(restoredDual.left.textColor,
             dualOverlay.left.textColor);
    QCOMPARE(restoredDual.left.verticalPlacement,
             dualOverlay.left.verticalPlacement);
    QCOMPARE(restoredDual.right.metrics,
             dualOverlay.right.metrics);
    QCOMPARE(restoredDual.right.badges,
             dualOverlay.right.badges);
    QCOMPARE(restoredDual.right.alignment,
             dualOverlay.right.alignment);
    QCOMPARE(restoredDual.right.textColor,
             dualOverlay.right.textColor);
    QCOMPARE(restoredDual.right.verticalPlacement,
             dualOverlay.right.verticalPlacement);

    PrinterProtocol::PaseOverlayConfig invalidColorOverlay =
        dualOverlay;
    invalidColorOverlay.left.textColor = 0x01000000U;
    QString invalidColorError;
    QVERIFY(!manager->persistPaseMetricsConfiguration(
        invalidColorOverlay, true, &invalidColorError));
    QVERIFY(!invalidColorError.isEmpty());
    PrinterProtocol::PaseOverlayConfig invalidPlacementOverlay =
        dualOverlay;
    invalidPlacementOverlay.right.verticalPlacement =
        QStringLiteral("Center");
    QString invalidPlacementError;
    QVERIFY(!manager->persistPaseMetricsConfiguration(
        invalidPlacementOverlay, true,
        &invalidPlacementError));
    QVERIFY(!invalidPlacementError.isEmpty());

    TryxRuntimeMetricsConfigRequest disableRequest;
    disableRequest.enabled = false;
    disableRequest.alignment = QStringLiteral("Left");
    disableRequest.textColor = 0xDCDCDCU;
    const QString disableId =
        QStringLiteral("21212121-2121-4121-8121-212121212121");
    QCOMPARE(manager->queueMetricsConfigOperation(disableId, disableRequest),
             disableId);
    manager->worker_->printerMetricsConfigured(
        disableId, true, PrinterProtocol::MutationOutcome::Succeeded,
        QString(), manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(disableId).state,
             QStringLiteral("Succeeded"));
    QVERIFY(!manager->metricsState().enabled);
    QVERIFY(!manager->metricsState().samplingActive);
    QVERIFY(manager->metricsState().metrics.isEmpty());
    QVERIFY(!QFileInfo::exists(manager->paseMetricsConfigPath()));
    QVERIFY(manager->persistedPaseOverlayForDevice(QStringLiteral("1-1"))
                .left.metrics
                .isEmpty());

    TryxRuntimeApplyRequest preserveRequest;
    preserveRequest.media = {
        QStringLiteral("existing.mp4.h264_2240x1080")};
    preserveRequest.ratio = QStringLiteral("2:1");
    preserveRequest.screenMode = QStringLiteral("Full Screen");
    preserveRequest.playMode = QStringLiteral("Single");
    QSignalSpy applySpy(manager.get(),
                        &DeviceManager::requestPrinterApplyMedia);
    const QString preserveId =
        QStringLiteral("22222222-2222-4222-8222-222222222223");
    QCOMPARE(manager->queueApplyOperation(preserveId, preserveRequest, false),
             preserveId);
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(applySpy.first().at(3).toBool(), false);
    manager->worker_->printerApplyFinished(
        preserveId, preserveRequest.media.first(), true, false,
        PrinterProtocol::MutationOutcome::Succeeded, QString(),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(preserveId).state,
             QStringLiteral("Succeeded"));
    QVERIFY(!manager->metricsState().enabled);

    const QString replaceId =
        QStringLiteral("23232323-2323-4323-8323-232323232323");
    TryxRuntimeApplyRequest replaceRequest = preserveRequest;
    replaceRequest.sysinfoLabels = {
        QStringLiteral("Voltage CPU")};
    replaceRequest.replaceOverlay = true;
    QCOMPARE(manager->queueApplyOperation(replaceId, replaceRequest, true),
             replaceId);
    QCOMPARE(manager->operationInfo(replaceId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(replaceId).errorCategory,
             QStringLiteral("UnsupportedConfiguration"));
}

void PrinterProtocolTests::
paseOverlayV1MigrationLoadsSingleArea() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QVERIFY(QDir().mkpath(
        QFileInfo(manager->paseMetricsConfigPath())
            .absolutePath()));
    QJsonObject legacy;
    legacy.insert(QStringLiteral("version"), 1);
    legacy.insert(QStringLiteral("enabled"), true);
    legacy.insert(
        QStringLiteral("deviceSerial"),
        QStringLiteral("PASE-LEGACY-OVERLAY"));
    legacy.insert(
        QStringLiteral("metrics"),
        QJsonArray{
            QStringLiteral("CPU Temperature"),
            QStringLiteral("Date&Time")});
    legacy.insert(
        QStringLiteral("alignment"),
        QStringLiteral("Center"));
    legacy.insert(
        QStringLiteral("textColor"),
        static_cast<qint64>(0x336699U));
    QVERIFY(writeTextFile(
        manager->paseMetricsConfigPath(),
        QJsonDocument(legacy).toJson(
            QJsonDocument::Compact)));

    manager->loadPaseMetricsConfig();
    const PrinterProtocol::PaseOverlayConfig migrated =
        manager->persistedPaseOverlayForDevice(
            QStringLiteral("PASE-LEGACY-OVERLAY"));
    QVERIFY(!migrated.dualMode);
    QVERIFY(!migrated.waterfallMode);
    QCOMPARE(
        migrated.left.metrics,
        QStringList({
            QStringLiteral("CPU Temperature"),
            QStringLiteral("Date&Time")}));
    QVERIFY(migrated.left.badges.isEmpty());
    QCOMPARE(migrated.left.alignment,
             QStringLiteral("Center"));
    QCOMPARE(migrated.left.textColor,
             0x336699U);
    QCOMPARE(migrated.left.verticalPlacement,
             QStringLiteral("Top"));
    QVERIFY(migrated.right.metrics.isEmpty());
    QVERIFY(migrated.right.badges.isEmpty());
}

void PrinterProtocolTests::paseSchedulerAcceptsSplitAndDisplayOnly() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestPrinterApplyMedia,
        manager->worker_, &DeviceWorker::applyPrinterMedia);
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;

    QSignalSpy applySpy(
        manager.get(), &DeviceManager::requestPrinterApplyMedia);
    TryxRuntimeApplyRequest split;
    split.media = {QStringLiteral("left.h264"),
                   QStringLiteral("right.h264")};
    split.ratio = QStringLiteral("2:1");
    split.screenMode = QStringLiteral("Screen Splitting");
    split.playMode = QStringLiteral("Single");
    split.sysinfoLabels = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Power")};
    split.settingsBadges = {
        QStringLiteral("CPU Badge")};
    split.sysinfoLabels2 = {
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Power")};
    split.settingsBadges2 = {
        QStringLiteral("GPU Badge")};
    split.settingsColor = QStringLiteral("#ff0000");
    split.settingsColor2 = QStringLiteral("#00ff00");
    split.replaceOverlay = true;
    const QString splitId =
        QStringLiteral("24242424-2424-4424-8424-242424242424");
    QCOMPARE(manager->queueApplyOperation(splitId, split, true),
             splitId);
    QCOMPARE(manager->operationInfo(splitId).state,
             QStringLiteral("Preflight"));
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(applySpy.first().at(3).toBool(), true);
    manager->worker_->printerApplyFinished(
        splitId, split.media.constFirst(), true, true,
        PrinterProtocol::MutationOutcome::Succeeded, QString(),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(splitId).state,
             QStringLiteral("Succeeded"));

    TryxRuntimeApplyRequest oneSided = split;
    oneSided.media.removeLast();
    const QString oneSidedId =
        QStringLiteral("25252525-2525-4525-8525-252525252526");
    QCOMPARE(manager->queueApplyOperation(
                 oneSidedId, oneSided, true),
             oneSidedId);
    QCOMPARE(manager->operationInfo(oneSidedId).errorCategory,
             QStringLiteral("UnsupportedConfiguration"));

    TryxRuntimeApplyRequest duplicateBadge = split;
    duplicateBadge.settingsBadges = {
        QStringLiteral("CPU Badge"),
        QStringLiteral("CPU Badge")};
    const QString duplicateBadgeId =
        QStringLiteral("26262626-2626-4626-8626-262626262626");
    QCOMPARE(manager->queueApplyOperation(
                 duplicateBadgeId, duplicateBadge, true),
             duplicateBadgeId);
    QCOMPARE(manager->operationInfo(duplicateBadgeId).errorCategory,
             QStringLiteral("UnsupportedConfiguration"));

    const QStringList invalidColors{
        QStringLiteral("red"),
        QStringLiteral("#f00"),
        QStringLiteral("#80ff0000"),
        QStringLiteral(" #ff0000")};
    int invalidColorIndex = 0;
    for (const QString &invalidColor : invalidColors) {
        TryxRuntimeApplyRequest invalidColorRequest = split;
        invalidColorRequest.settingsColor = invalidColor;
        const QString invalidColorId =
            QStringLiteral(
                "27272727-2727-4727-8727-%1")
                .arg(
                    272727272720ULL +
                    static_cast<quint64>(
                        invalidColorIndex++),
                    12, 10, QLatin1Char('0'));
        QCOMPARE(
            manager->queueApplyOperation(
                invalidColorId, invalidColorRequest, true),
            invalidColorId);
        QCOMPARE(
            manager->operationInfo(
                invalidColorId).errorCategory,
            QStringLiteral("UnsupportedConfiguration"));
    }

    TryxRuntimeApplyRequest invalidBrightness;
    invalidBrightness.display.brightnessPresent = true;
    invalidBrightness.display.brightness = 101;
    const QString invalidBrightnessId =
        QStringLiteral("27272727-2727-4727-8727-272727272727");
    QCOMPARE(manager->queueApplyOperation(
                 invalidBrightnessId, invalidBrightness),
             invalidBrightnessId);
    QCOMPARE(
        manager->operationInfo(invalidBrightnessId).errorCategory,
        QStringLiteral("UnsupportedConfiguration"));

    TryxRuntimeApplyRequest invalidStandby;
    invalidStandby.display.standbyPresent = true;
    invalidStandby.display.standbyEnabled = false;
    const QString invalidStandbyId =
        QStringLiteral("27272727-2727-4727-8727-272727272728");
    QCOMPARE(manager->queueApplyOperation(
                 invalidStandbyId, invalidStandby),
             invalidStandbyId);
    QCOMPARE(
        manager->operationInfo(invalidStandbyId).errorCategory,
        QStringLiteral("UnsupportedConfiguration"));

    applySpy.clear();
    TryxRuntimeApplyRequest displayOnly;
    displayOnly.display.brightnessPresent = true;
    displayOnly.display.brightness = 42;
    displayOnly.display.backlightPresent = true;
    displayOnly.display.backlightEnabled = false;
    const QString displayOnlyId =
        QStringLiteral("28282828-2828-4828-8828-282828282828");
    QCOMPARE(manager->queueApplyOperation(
                 displayOnlyId, displayOnly),
             displayOnlyId);
    QCOMPARE(manager->operationInfo(displayOnlyId).state,
             QStringLiteral("Preflight"));
    QCOMPARE(applySpy.count(), 1);
    QVERIFY(applySpy.first().at(1).toString().isEmpty());
    QCOMPARE(applySpy.first().at(3).toBool(), false);
    const TryxRuntimeApplyRequest dispatched =
        qvariant_cast<TryxRuntimeApplyRequest>(
            applySpy.first().at(2));
    QVERIFY(dispatched.display.backlightPresent);
    QVERIFY(!dispatched.display.backlightEnabled);
    manager->worker_->printerApplyFinished(
        displayOnlyId, QString(), true, false,
        PrinterProtocol::MutationOutcome::Succeeded, QString(),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(displayOnlyId).state,
             QStringLiteral("Succeeded"));
}

void PrinterProtocolTests::mediaCatalogPersistsThumbnailAndPrunesAuthoritatively() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->printerDeviceSerial_ = QStringLiteral("PASE-CATALOG-1");

    const QString stagedDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(stagedDirectory));
    const QString stagedPath =
        QDir(stagedDirectory).filePath(QStringLiteral("preview.jpg"));
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#336699")));
    QVERIFY(image.save(stagedPath, "JPG", 90));
    QFile stagedFile(stagedPath);
    QVERIFY(stagedFile.open(QIODevice::ReadOnly));
    const QString stagedHash = QString::fromLatin1(
        QCryptographicHash::hash(stagedFile.readAll(),
                                 QCryptographicHash::Sha256)
            .toHex());

    const QString operationId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.stagedThumbnailPath = stagedPath;
    record.stagedThumbnailSha256 = stagedHash;
    manager->operations_.insert(operationId, record);

    TryxRuntimeMediaEntry verified;
    verified.name = QStringLiteral("upload.mp4.h264_2240x1080");
    verified.size = 123456;
    verified.source = 1;
    verified.readOnly = false;
    const QString key =
        manager->promoteThumbnailForOperation(operationId, verified);
    QVERIFY(!key.isEmpty());
    const QString persistentPath = manager->mediaThumbnailPath(key);
    QVERIFY(!persistentPath.isEmpty());
    QVERIFY(QFileInfo::exists(manager->mediaCatalogIndexPath()));

    PrinterProtocol::MediaFile media;
    media.name = verified.name;
    media.size = static_cast<quint32>(verified.size);
    media.source = PrinterProtocol::MediaSource::User;
    media.readOnly = false;
    manager->updateMediaCatalog({media});
    QCOMPARE(manager->mediaCatalogSnapshot().entries.size(), 1);
    QCOMPARE(manager->mediaCatalogSnapshot().entries.first().thumbnailKey,
             key);

    manager->clearMediaCatalogView();
    QVERIFY(QFileInfo::exists(persistentPath));
    manager->mediaCatalogIndex_ = {};
    manager->loadMediaCatalogIndex();
    manager->updateMediaCatalog({media});
    QCOMPARE(manager->mediaCatalogSnapshot().entries.first().thumbnailKey,
             key);

    manager->updateMediaCatalog({});
    QVERIFY(!QFileInfo::exists(persistentPath));
    QVERIFY(manager->mediaCatalogSnapshot().entries.isEmpty());
}

void PrinterProtocolTests::mediaCatalogV1MigrationStripsForgedOrigin() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString catalogDirectory =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("media-catalog"));
    const QString thumbnailDirectory =
        QDir(catalogDirectory).filePath(QStringLiteral("thumbnails"));
    QVERIFY(QDir().mkpath(thumbnailDirectory));

    const QString deviceIdentity = QStringLiteral("PASE-V1");
    const QString name = QStringLiteral("legacy.mp4.h264_2240x1080");
    constexpr quint64 size = 9876;
    const QByteArray identity =
        deviceIdentity.toUtf8() + '\0' + name.toUtf8() + '\0' +
        QByteArray::number(size) + '\0' + QByteArrayLiteral("1");
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
            .toHex());
    const QString thumbnailPath =
        QDir(thumbnailDirectory).filePath(key + QStringLiteral(".jpg"));
    QImage image(12, 12, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#225588")));
    QVERIFY(image.save(thumbnailPath, "JPG", 90));
    QFile thumbnailFile(thumbnailPath);
    QVERIFY(thumbnailFile.open(QIODevice::ReadOnly));
    const QString thumbnailSha = QString::fromLatin1(
        QCryptographicHash::hash(thumbnailFile.readAll(),
                                 QCryptographicHash::Sha256)
            .toHex());

    QJsonObject v1Record;
    v1Record.insert(QStringLiteral("deviceIdentity"), deviceIdentity);
    v1Record.insert(QStringLiteral("name"), name);
    v1Record.insert(QStringLiteral("size"), QString::number(size));
    v1Record.insert(QStringLiteral("source"), 1);
    v1Record.insert(QStringLiteral("readOnly"), false);
    v1Record.insert(QStringLiteral("sha256"), thumbnailSha);
    v1Record.insert(QStringLiteral("sourceContentSha256"),
                    QString(64, QLatin1Char('a')));
    v1Record.insert(QStringLiteral("sourceSize"), QStringLiteral("123"));
    v1Record.insert(QStringLiteral("preparedSha256"),
                    QString(64, QLatin1Char('b')));
    v1Record.insert(QStringLiteral("conversionProfile"),
                    QStringLiteral("forged-profile"));
    QJsonObject entries;
    entries.insert(key, v1Record);
    QJsonObject v1Root;
    v1Root.insert(QStringLiteral("version"), 1);
    v1Root.insert(QStringLiteral("entries"), entries);
    QVERIFY(writeTextFile(
        QDir(catalogDirectory).filePath(QStringLiteral("index.json")),
        QJsonDocument(v1Root).toJson(QJsonDocument::Compact)));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QFile migrated(manager->mediaCatalogIndexPath());
    QVERIFY(migrated.open(QIODevice::ReadOnly));
    const QJsonObject migratedRoot =
        QJsonDocument::fromJson(migrated.readAll()).object();
    QCOMPARE(migratedRoot.value(QStringLiteral("version")).toInt(), 2);
    const QJsonObject migratedRecord =
        migratedRoot.value(QStringLiteral("entries"))
            .toObject().value(key).toObject();
    QCOMPARE(migratedRecord.value(QStringLiteral("thumbnailSha256"))
                 .toString(),
             thumbnailSha);
    QVERIFY(!migratedRecord.contains(QStringLiteral("sha256")));
    QVERIFY(!migratedRecord.contains(
        QStringLiteral("sourceContentSha256")));
    QVERIFY(!migratedRecord.contains(QStringLiteral("preparedSha256")));
    QVERIFY(!migratedRecord.contains(QStringLiteral("conversionProfile")));

    QFile rollback(QDir(catalogDirectory).filePath(
        QStringLiteral("index.v1.rollback.json")));
    QVERIFY(rollback.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(rollback.readAll()).object(), v1Root);
}

void PrinterProtocolTests::mediaCatalogV1MigrationRejectsStaleRollback() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString catalogDirectory =
        QDir(temporaryDirectory.path()).filePath(
            QStringLiteral("media-catalog"));
    QVERIFY(QDir().mkpath(catalogDirectory));

    QJsonObject currentEntries;
    currentEntries.insert(QString(64, QLatin1Char('a')), QJsonObject{});
    QJsonObject currentRoot;
    currentRoot.insert(QStringLiteral("version"), 1);
    currentRoot.insert(QStringLiteral("entries"), currentEntries);
    QJsonObject staleRoot;
    staleRoot.insert(QStringLiteral("version"), 1);
    staleRoot.insert(QStringLiteral("entries"), QJsonObject{});
    const QString indexPath =
        QDir(catalogDirectory).filePath(QStringLiteral("index.json"));
    QVERIFY(writeTextFile(
        indexPath,
        QJsonDocument(currentRoot).toJson(QJsonDocument::Compact)));
    QVERIFY(writeTextFile(
        QDir(catalogDirectory).filePath(
            QStringLiteral("index.v1.rollback.json")),
        QJsonDocument(staleRoot).toJson(QJsonDocument::Compact)));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QVERIFY(!manager->mediaCatalogWriteEnabled_);
    QFile index(indexPath);
    QVERIFY(index.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(index.readAll()).object(), currentRoot);
}

void PrinterProtocolTests::ensureMediaReusesOriginWithoutPreparation() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    QVERIFY(!manager->printerDeviceSerial_.trimmed().isEmpty());

    const QString sourcePath =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("source.mp4"));
    const QByteArray sourceBytes("same-content-for-ensure");
    QVERIFY(writeTextFile(sourcePath, sourceBytes));
    const QString sourceSha = QString::fromLatin1(
        QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256)
            .toHex());
    const QString profile = QStringLiteral("pase-profile-test");
    PrinterProtocol::MediaFile remote;
    remote.name = QStringLiteral("existing.mp4.h264_2240x1080");
    remote.size = 6125;
    remote.source = PrinterProtocol::MediaSource::User;
    remote.readOnly = false;
    TryxRuntimeMediaEntry identityEntry;
    identityEntry.name = remote.name;
    identityEntry.size = remote.size;
    identityEntry.source = 1;
    identityEntry.readOnly = false;
    const QString key = manager->mediaThumbnailKey(
        manager->printerDeviceSerial_, identityEntry);
    QJsonObject origin;
    origin.insert(QStringLiteral("deviceIdentity"),
                  manager->printerDeviceSerial_);
    origin.insert(QStringLiteral("name"), remote.name);
    origin.insert(QStringLiteral("size"), QString::number(remote.size));
    origin.insert(QStringLiteral("source"), 1);
    origin.insert(QStringLiteral("readOnly"), false);
    origin.insert(QStringLiteral("sourceContentSha256"), sourceSha);
    origin.insert(QStringLiteral("sourceSize"),
                  QString::number(sourceBytes.size()));
    origin.insert(QStringLiteral("preparedSha256"),
                  QString(64, QLatin1Char('d')));
    origin.insert(QStringLiteral("conversionProfile"), profile);
    manager->mediaCatalogIndex_.insert(key, origin);

    QObject::disconnect(manager.get(),
                        &DeviceManager::requestAnalyzePrinterSource,
                        manager->printerMediaPreparer_,
                        &PrinterMediaPreparer::analyzeSource);
    QObject::disconnect(manager.get(),
                        &DeviceManager::requestPrinterRefreshMedia,
                        manager->worker_,
                        &DeviceWorker::refreshPrinterMediaList);
    QObject::disconnect(manager.get(),
                        &DeviceManager::requestPrinterApplyMedia,
                        manager->worker_, &DeviceWorker::applyPrinterMedia);
    QSignalSpy analyzeSpy(manager.get(),
                         &DeviceManager::requestAnalyzePrinterSource);
    QSignalSpy prepareSpy(manager.get(),
                         &DeviceManager::requestPreparePrinterMedia);
    QSignalSpy uploadSpy(manager.get(),
                        &DeviceManager::requestPrinterUploadPrepared);
    QSignalSpy refreshSpy(manager.get(),
                         &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy applySpy(manager.get(),
                       &DeviceManager::requestPrinterApplyMedia);

    TryxRuntimeApplyRequest request;
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Single");
    request.ratio = QStringLiteral("2:1");
    const QString operationId =
        manager->queueEnsureMediaAndApplyOperation(
            QStringLiteral("14141414-1414-4414-8414-141414141414"),
            sourcePath, request);
    QCOMPARE(analyzeSpy.count(), 1);
    emit manager->printerMediaPreparer_->sourceAnalyzed(
        operationId, sourcePath, sourceSha, sourceBytes.size(), profile,
        manager->printerGeneration_);
    QCOMPARE(refreshSpy.count(), 1);
    emit manager->worker_->printerMediaListReady(
        operationId, QList<PrinterProtocol::MediaFile>{remote},
        manager->printerGeneration_);
    QCOMPARE(prepareSpy.count(), 0);
    QCOMPARE(uploadSpy.count(), 0);
    QCOMPARE(applySpy.count(), 1);
    QCOMPARE(applySpy.first().at(1).toString(), remote.name);
    emit manager->worker_->printerApplyFinished(
        operationId, remote.name, true, false,
        PrinterProtocol::MutationOutcome::Succeeded, QString(),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Succeeded"));
}

void PrinterProtocolTests::ensureOriginMissReleasesForegroundBeforePreparation() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    manager->printerDisplaySessionActive_ = true;

    const QString sourcePath =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("miss.mp4"));
    const QByteArray sourceBytes("origin-miss-content");
    QVERIFY(writeTextFile(sourcePath, sourceBytes));
    const QString sourceSha = QString::fromLatin1(
        QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256)
            .toHex());
    const QString profile = QStringLiteral("pase-profile-test");

    QObject::disconnect(manager.get(),
                        &DeviceManager::requestAnalyzePrinterSource,
                        manager->printerMediaPreparer_,
                        &PrinterMediaPreparer::analyzeSource);
    QObject::disconnect(manager.get(),
                        &DeviceManager::requestPrinterRefreshMedia,
                        manager->worker_,
                        &DeviceWorker::refreshPrinterMediaList);
    QObject::disconnect(manager.get(),
                        &DeviceManager::requestPreparePrinterMedia,
                        manager->printerMediaPreparer_,
                        &PrinterMediaPreparer::startPreparation);
    QStringList transitions;
    connect(manager.get(), &DeviceManager::requestBeginPrinterForegroundOperation,
            manager.get(), [&transitions](const QString &, quint64) {
                transitions.append(QStringLiteral("begin"));
            });
    connect(manager.get(), &DeviceManager::requestEndPrinterForegroundOperation,
            manager.get(), [&transitions](const QString &, quint64) {
                transitions.append(QStringLiteral("end"));
            });
    connect(manager.get(), &DeviceManager::requestPreparePrinterMedia,
            manager.get(), [&transitions](const QString &, const QString &,
                                           const QString &, const QString &,
                                           quint64) {
                transitions.append(QStringLiteral("prepare"));
            });

    TryxRuntimeApplyRequest request;
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Single");
    request.ratio = QStringLiteral("2:1");
    const QString operationId =
        manager->queueEnsureMediaAndApplyOperation(
            QStringLiteral("15151515-1515-4515-8515-151515151515"),
            sourcePath, request);
    emit manager->printerMediaPreparer_->sourceAnalyzed(
        operationId, sourcePath, sourceSha, sourceBytes.size(), profile,
        manager->printerGeneration_);
    emit manager->worker_->printerMediaListReady(
        operationId, {}, manager->printerGeneration_);
    QCOMPARE(transitions.count(QStringLiteral("begin")), 1);
    QCOMPARE(transitions.count(QStringLiteral("end")), 1);
    QCOMPARE(transitions.count(QStringLiteral("prepare")), 1);
    QVERIFY(transitions.indexOf(QStringLiteral("end")) <
            transitions.indexOf(QStringLiteral("prepare")));
    manager->cancelOperation(operationId);
}

void PrinterProtocolTests::previewCommitFailureRemainsRetryable_data() {
    QTest::addColumn<bool>("finalizationRecovery");

    QTest::newRow("regular-upload") << false;
    QTest::newRow("finalization-recovery") << true;
}

void PrinterProtocolTests::previewCommitFailureRemainsRetryable() {
    QFETCH(bool, finalizationRecovery);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());

    manager->mediaCatalogDirectoryOverride_ =
        QDir(temporaryDirectory.path())
            .filePath(QStringLiteral("catalog-blocker"));
    QFile catalogBlocker(manager->mediaCatalogDirectory());
    QVERIFY(catalogBlocker.open(QIODevice::WriteOnly));
    QCOMPARE(catalogBlocker.write(QByteArrayLiteral("not-a-directory")), 15);
    catalogBlocker.close();

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("verified.h264"));
    const QByteArray preparedBytes(8192, '\x61');
    QFile prepared(preparedPath);
    QVERIFY(prepared.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(prepared.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    prepared.close();

    const QString stagedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("verified.jpg"));
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#663399")));
    QVERIFY(image.save(stagedPath, "JPG", 90));
    QFile staged(stagedPath);
    QVERIFY(staged.open(QIODevice::ReadOnly));
    const QString stagedHash = QString::fromLatin1(
        QCryptographicHash::hash(staged.readAll(),
                                 QCryptographicHash::Sha256)
            .toHex());

    const QString operationId =
        QStringLiteral("30303030-3030-4030-8030-303030303030");
    const QString remoteName =
        QStringLiteral("verified.mp4.h264_2240x1080");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RefreshingMedia");
    if (finalizationRecovery) {
        record.info.errorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.terminalOutcome =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorMessage =
            QStringLiteral("FileTransmitEnd status timed out");
    }
    record.info.total = preparedBytes.size();
    record.info.confirmedBytes = preparedBytes.size();
    record.info.lastConfirmedChunkIndex = 0;
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    record.stagedThumbnailPath = stagedPath;
    record.stagedThumbnailSha256 = stagedHash;
    record.remoteName = remoteName;
    record.originalRemoteName = remoteName;
    record.uploadDeviceIdentity =
        manager->printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration =
        manager->printerGeneration_;
    record.uploadFinalizationReconciliationPending =
        finalizationRecovery;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    PrinterProtocol::MediaFile uploaded;
    uploaded.name = remoteName;
    uploaded.size = static_cast<quint32>(preparedBytes.size());
    uploaded.source = PrinterProtocol::MediaSource::User;
    uploaded.readOnly = false;
    manager->worker_->printerMediaListReady(
        operationId, {uploaded}, manager->printerGeneration_);

    const TryxRuntimeOperationInfo result =
        manager->operationInfo(operationId);
    QCOMPARE(result.state, QStringLiteral("RetryAvailable"));
    QCOMPARE(result.retryMode, QStringLiteral("PreparedMedia"));
    QCOMPARE(result.terminalOutcome, QStringLiteral("NotStarted"));
    QCOMPARE(result.primaryErrorCategory,
             finalizationRecovery
                 ? QStringLiteral("FinalizationUnknown")
                 : QStringLiteral("NotStarted"));
    QVERIFY(result.message.contains(QStringLiteral("preview"),
                                    Qt::CaseInsensitive));
    QVERIFY(!manager->operations_.value(operationId)
                 .retryMustUseNewRemoteName);
    QCOMPARE(manager->retryCacheOperationId_, operationId);
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(QFileInfo::exists(preparedPath));
    QVERIFY(QFileInfo::exists(stagedPath));
    QCOMPARE(manager->mediaCatalogSnapshot().entries.size(), 1);
    QVERIFY(manager->mediaCatalogSnapshot()
                .entries.first().thumbnailKey.isEmpty());

    QFile retryManifest(manager->retryCacheManifestPath());
    QVERIFY(retryManifest.open(QIODevice::ReadOnly));
    const QJsonObject retryManifestObject =
        QJsonDocument::fromJson(retryManifest.readAll()).object();
    QCOMPARE(
        retryManifestObject.value(QStringLiteral("confirmedBytes"))
            .toString(),
        QStringLiteral("0"));
    QCOMPARE(
        retryManifestObject
            .value(QStringLiteral("lastConfirmedChunkIndex"))
            .toInteger(),
        -1);
    retryManifest.close();

    QVERIFY(QFile::remove(manager->mediaCatalogDirectory()));
    QVERIFY(QDir().mkpath(manager->mediaCatalogDirectory()));
    manager.reset();

    std::unique_ptr<DeviceManager> restarted(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        restarted.get(),
        &DeviceManager::requestValidatePrinterRetryCache,
        restarted->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    QObject::disconnect(
        restarted.get(), &DeviceManager::requestStartPrinterSession,
        restarted->worker_, &DeviceWorker::startPrinterDisplaySession);
    restarted->loadRetryCache();
    const QString validationId =
        restarted->pendingRetryValidationId_;
    QVERIFY(!validationId.isEmpty());
    restarted->setAutoConnectModeForTesting(true);
    restarted->rescanPrinterForTesting();
    restarted->printerDisplaySessionActive_ = true;
    restarted->handleRetryCacheValidation(
        validationId, true, false, QString());
    const QString restoredId = restarted->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    QCOMPARE(restarted->operationInfo(restoredId).terminalOutcome,
             QStringLiteral("NotStarted"));

    QSignalSpy refreshSpy(
        restarted.get(), &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy uploadSpy(
        restarted.get(), &DeviceManager::requestPrinterUploadPrepared);
    const QString retryId =
        QStringLiteral("31313131-3131-4131-8131-313131313131");
    QCOMPARE(restarted->retryOperation(restoredId, retryId), retryId);
    restarted->printerMediaPreparer_->retryCacheValidated(
        retryId, true, false, QString());
    QCOMPARE(refreshSpy.count(), 1);
    restarted->worker_->printerMediaListReady(
        retryId, {uploaded}, restarted->printerGeneration_);

    QCOMPARE(uploadSpy.count(), 0);
    QCOMPARE(restarted->operationInfo(retryId).state,
             QStringLiteral("Succeeded"));
    QVERIFY(!restarted->mediaCatalogSnapshot()
                 .entries.first().thumbnailKey.isEmpty());
    QVERIFY(!QFileInfo::exists(restarted->retryCacheManifestPath()));
    QVERIFY(!QFileInfo::exists(preparedPath));
}

void PrinterProtocolTests::successfulUploadDoesNotDeleteUnrelatedRetryCandidate() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString oldPreparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("old-retry.h264"));
    const QByteArray oldBytes(4096, '\x31');
    QFile oldPrepared(oldPreparedPath);
    QVERIFY(oldPrepared.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(oldPrepared.write(oldBytes),
             static_cast<qint64>(oldBytes.size()));
    oldPrepared.close();

    const QString oldId =
        QStringLiteral("24242424-2424-4424-8424-242424242424");
    DeviceManager::OperationRecord oldRecord;
    oldRecord.info.id = oldId;
    oldRecord.info.kind = QStringLiteral("Upload");
    oldRecord.info.state = QStringLiteral("RetryAvailable");
    oldRecord.info.stage = QStringLiteral("RetryAvailable");
    oldRecord.info.errorCategory = QStringLiteral("PartialOrUnknown");
    oldRecord.info.retryMode = QStringLiteral("PreparedMedia");
    oldRecord.preparedPath = oldPreparedPath;
    oldRecord.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(oldBytes, QCryptographicHash::Sha256)
            .toHex());
    oldRecord.remoteName =
        QStringLiteral("old-retry.mp4.h264_2240x1080");
    oldRecord.uploadDeviceIdentity =
        manager->printerDeviceSerial_.trimmed();
    oldRecord.uploadDeviceGeneration =
        manager->printerGeneration_;
    manager->operations_.insert(oldId, oldRecord);
    manager->operationOrder_.append(oldId);
    QString cacheError;
    QVERIFY2(manager->writeRetryCache(
                 oldId, QStringLiteral("PartialOrUnknown"), &cacheError),
             qPrintable(cacheError));

    const QString newPreparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("new-success.h264"));
    const QByteArray newBytes(8192, '\x52');
    QFile newPrepared(newPreparedPath);
    QVERIFY(newPrepared.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(newPrepared.write(newBytes),
             static_cast<qint64>(newBytes.size()));
    newPrepared.close();

    const QString newId =
        QStringLiteral("25252525-2525-4525-8525-252525252525");
    const QString newName =
        QStringLiteral("new-success.mp4.h264_2240x1080");
    DeviceManager::OperationRecord newRecord;
    newRecord.info.id = newId;
    newRecord.info.kind = QStringLiteral("Upload");
    newRecord.info.state = QStringLiteral("Refreshing");
    newRecord.info.stage = QStringLiteral("RefreshingMedia");
    newRecord.info.deviceGeneration = manager->printerGeneration_;
    newRecord.preparedPath = newPreparedPath;
    newRecord.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(newBytes, QCryptographicHash::Sha256)
            .toHex());
    newRecord.remoteName = newName;
    manager->operations_.insert(newId, newRecord);
    manager->operationOrder_.append(newId);
    manager->activeOperationId_ = newId;

    PrinterProtocol::MediaFile uploaded;
    uploaded.name = newName;
    uploaded.size = static_cast<quint32>(newBytes.size());
    uploaded.source = PrinterProtocol::MediaSource::User;
    uploaded.readOnly = false;
    manager->worker_->printerMediaListReady(
        newId, {uploaded}, manager->printerGeneration_);

    QCOMPARE(manager->operationInfo(newId).state,
             QStringLiteral("Succeeded"));
    QCOMPARE(manager->operationInfo(oldId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->retryCacheOperationId_, oldId);
    QCOMPARE(manager->retryCachePreparedPath_, oldPreparedPath);
    QVERIFY(QFileInfo::exists(oldPreparedPath));
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(!QFileInfo::exists(newPreparedPath));
}

void PrinterProtocolTests::schedulerRejectsConcurrentOperationAndKeepsExactIdentity() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    QTemporaryFile source;
    QVERIFY(source.open());
    const QByteArray sourceBytes =
        QByteArrayLiteral("not-decoded-before-assertions");
    QCOMPARE(source.write(sourceBytes), static_cast<qint64>(sourceBytes.size()));
    source.flush();

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    QSignalSpy changedSpy(manager.get(), &DeviceManager::operationChanged);

    const QString firstId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString secondId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    QCOMPARE(manager->queueUploadOperation(firstId, source.fileName(), false),
             firstId);
    QCOMPARE(manager->activeOperationInfo().id, firstId);
    QCOMPARE(manager->activeOperationInfo().state,
             QStringLiteral("Converting"));

    QCOMPARE(manager->queueUploadOperation(secondId, source.fileName(), false),
             secondId);
    const TryxRuntimeOperationInfo rejected = manager->operationInfo(secondId);
    QCOMPARE(rejected.id, secondId);
    QCOMPARE(rejected.state, QStringLiteral("Failed"));
    QCOMPARE(rejected.stage, QStringLiteral("Rejected"));
    QCOMPARE(rejected.errorCategory, QStringLiteral("Busy"));
    QCOMPARE(manager->activeOperationInfo().id, firstId);

    const int signalCountBeforeDuplicate = changedSpy.count();
    QCOMPARE(manager->queueUploadOperation(firstId, source.fileName(), true),
             firstId);
    QCOMPARE(changedSpy.count(), signalCountBeforeDuplicate);
    QCOMPARE(manager->operationInfo(firstId).kind, QStringLiteral("Upload"));

    manager->cancelOperation(firstId);
    QCOMPARE(manager->operationInfo(firstId).state,
             QStringLiteral("Cancelled"));
    QVERIFY(manager->activeOperationInfo().id.isEmpty());

    const TryxRuntimeOperationsSnapshot snapshot = manager->operationSnapshot();
    QCOMPARE(snapshot.activeOperationId, QString());
    QCOMPARE(snapshot.operations.size(), 2);
    QVERIFY(snapshot.revision >= 3);
}

void PrinterProtocolTests::uploadWithoutDeviceIdentityIsRejected() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString usbName = QStringLiteral("1-1");
    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(QFile::remove(
        QDir(sysRoot)
            .filePath(QStringLiteral("bus/usb/devices/") + usbName +
                      QStringLiteral("/serial"))));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName,
                                  QStringLiteral("lp0")));

    QTemporaryFile source;
    QVERIFY(source.open());
    QCOMPARE(source.write(QByteArrayLiteral("media")), 5);
    source.flush();

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    QVERIFY(manager->printerDeviceSerial_.isEmpty());

    const QString operationId =
        QStringLiteral("12121212-1212-4212-8212-121212121212");
    QCOMPARE(manager->queueUploadOperation(
                 operationId, source.fileName(), false),
             operationId);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("DeviceIdentityUnavailable"));
    QVERIFY(manager->activeOperationInfo().id.isEmpty());
}

void PrinterProtocolTests::queuedApplyCancellationIsNotDrainedOrExecuted() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;
    QSignalSpy changedSpy(manager.get(), &DeviceManager::operationChanged);

    TryxRuntimeApplyRequest request;
    request.media = {QStringLiteral("existing.mp4.h264_2240x1080")};
    request.ratio = QStringLiteral("2:1");
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Single");
    const QString operationId =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    QCOMPARE(manager->queueApplyOperation(operationId, request), operationId);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Preflight"));

    manager->cancelOperation(operationId);
    QTRY_COMPARE(manager->operationInfo(operationId).state,
                 QStringLiteral("Cancelled"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("UserCancelled"));
    QVERIFY(manager->activeOperationInfo().id.isEmpty());

    int terminalSignals = 0;
    for (const QList<QVariant> &arguments : changedSpy) {
        const TryxRuntimeOperationInfo info =
            qvariant_cast<TryxRuntimeOperationInfo>(arguments.at(0));
        if (info.id == operationId &&
            (info.state == QStringLiteral("Succeeded") ||
             info.state == QStringLiteral("Failed") ||
             info.state == QStringLiteral("Cancelled") ||
             info.state == QStringLiteral("RetryAvailable"))) {
            ++terminalSignals;
        }
    }
    QCOMPARE(terminalSignals, 1);
}

void PrinterProtocolTests::retryPreflightAlwaysUsesFreshNameAfterPartialUpload() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    manager->printerDisplaySessionActive_ = true;

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("prepared.h264"));
    const QByteArray preparedBytes(4096, '\x71');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString sourceId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    const QString retryId =
        QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QString uncertainName =
        QStringLiteral("uncertain.mp4.h264_2240x1080");
    DeviceManager::OperationRecord sourceRecord;
    sourceRecord.info.id = sourceId;
    sourceRecord.info.kind = QStringLiteral("Upload");
    sourceRecord.info.state = QStringLiteral("RetryAvailable");
    sourceRecord.info.stage = QStringLiteral("RetryAvailable");
    sourceRecord.info.errorCategory = QStringLiteral("PartialOrUnknown");
    sourceRecord.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
    sourceRecord.info.primaryErrorCategory =
        QStringLiteral("PartialOrUnknown");
    sourceRecord.info.primaryErrorMessage =
        QStringLiteral("DataStatus timeout after the third data request");
    sourceRecord.info.retryMode = QStringLiteral("PreparedMedia");
    sourceRecord.info.subject = QStringLiteral("source.mp4");
    sourceRecord.info.resultName = uncertainName;
    sourceRecord.info.total = preparedBytes.size();
    sourceRecord.info.confirmedBytes = 2048;
    sourceRecord.info.lastConfirmedChunkIndex = 0;
    sourceRecord.info.deviceGeneration = manager->printerGeneration_;
    sourceRecord.preparedPath = preparedPath;
    sourceRecord.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes, QCryptographicHash::Sha256)
            .toHex());
    sourceRecord.remoteName = uncertainName;
    sourceRecord.originalRemoteName = uncertainName;
    sourceRecord.retryMustUseNewRemoteName = true;
    sourceRecord.uploadDeviceIdentity =
        manager->printerDeviceSerial_.trimmed();
    sourceRecord.uploadDeviceGeneration =
        manager->printerGeneration_;
    manager->operations_.insert(sourceId, sourceRecord);
    manager->operationOrder_.append(sourceId);
    QString cacheError;
    QVERIFY2(manager->writeRetryCache(sourceId,
                                      QStringLiteral("PartialOrUnknown"),
                                      &cacheError),
             qPrintable(cacheError));

    QSignalSpy removedSpy(manager.get(), &DeviceManager::operationRemoved);
    QSignalSpy refreshSpy(manager.get(),
                          &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy uploadSpy(manager.get(),
                         &DeviceManager::requestPrinterUploadPrepared);
    QCOMPARE(manager->retryOperation(sourceId, retryId), retryId);
    QCOMPARE(refreshSpy.count(), 0);
    QCOMPARE(manager->operations_.value(retryId).remoteName, uncertainName);
    QVERIFY(!manager->operations_.contains(sourceId));
    QCOMPARE(manager->retryCacheOperationId_, retryId);
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.first().at(0).toString(), sourceId);

    manager->printerMediaPreparer_->retryCacheValidated(
        retryId, true, false, QString());
    QCOMPARE(refreshSpy.count(), 1);

    PrinterProtocol::MediaFile existingFile;
    existingFile.name = uncertainName;
    existingFile.size = static_cast<quint32>(preparedBytes.size());
    existingFile.source = PrinterProtocol::MediaSource::User;
    manager->worker_->printerMediaListReady(
        retryId, {existingFile}, manager->printerGeneration_);
    QCOMPARE(uploadSpy.count(), 1);
    const QString replacementName =
        uploadSpy.first().at(2).toString();
    QVERIFY(replacementName != uncertainName);
    QCOMPARE(manager->operations_.value(retryId).remoteName,
             replacementName);
    QVERIFY(manager->operationInfo(retryId).terminalOutcome.isEmpty());
    QCOMPARE(manager->operationInfo(retryId).primaryErrorMessage,
             sourceRecord.info.primaryErrorMessage);
    QCOMPARE(manager->operationInfo(retryId).confirmedBytes, 0);
    QCOMPARE(manager->operationInfo(retryId).lastConfirmedChunkIndex,
             -1);
    QVERIFY(QFileInfo::exists(preparedPath));
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));
    QFile updatedManifest(manager->retryCacheManifestPath());
    QVERIFY(updatedManifest.open(QIODevice::ReadOnly));
    const QJsonObject updatedManifestObject =
        QJsonDocument::fromJson(updatedManifest.readAll()).object();
    QCOMPARE(updatedManifestObject
                 .value(QStringLiteral("originalRemoteName"))
                 .toString(),
             uncertainName);
    QCOMPARE(updatedManifestObject
                 .value(QStringLiteral("retryRemoteName"))
                 .toString(),
             replacementName);
    QVERIFY(updatedManifestObject
                .value(QStringLiteral("requiresNewRemoteName"))
                .toBool());
}

void PrinterProtocolTests::startupRetryCacheValidationSerializesMutations() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));

    const QString cachedOperationId =
        QStringLiteral("77777777-7777-4777-8777-777777777777");
    QString preparedPath;
    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        preparedPath = QDir(cacheDirectory).filePath(
            QStringLiteral("startup-cache.h264"));
        const QByteArray preparedBytes(8192, '\x45');
        QFile preparedFile(preparedPath);
        QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                                  QIODevice::Truncate));
        QCOMPARE(preparedFile.write(preparedBytes),
                 static_cast<qint64>(preparedBytes.size()));
        preparedFile.close();

        DeviceManager::OperationRecord record;
        record.info.id = cachedOperationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.errorCategory = QStringLiteral("PartialOrUnknown");
        record.info.retryMode = QStringLiteral("PreparedMedia");
        record.info.subject = QStringLiteral("startup-source.mp4");
        record.info.resultName =
            QStringLiteral("startup-source.mp4.h264_2240x1080");
        record.info.total = preparedBytes.size();
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = record.info.resultName;
        record.uploadDeviceIdentity = QStringLiteral("1-1");
        record.uploadDeviceGeneration = 1;
        writer->operations_.insert(cachedOperationId, record);
        writer->operationOrder_.append(cachedOperationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     cachedOperationId, QStringLiteral("PartialOrUnknown"),
                     &cacheError),
                 qPrintable(cacheError));

        QFile orphan(QDir(cacheDirectory).filePath(
            QStringLiteral("orphan.h264")));
        QVERIFY(orphan.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(orphan.write(QByteArrayLiteral("orphan")), 6);
    }

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    QSignalSpy validationSpy(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache);

    manager->loadRetryCache();
    QCOMPARE(validationSpy.count(), 1);
    QCOMPARE(validationSpy.first().at(0).toString(), cachedOperationId);
    QCOMPARE(manager->pendingRetryValidationId_, cachedOperationId);
    QVERIFY(!manager->operations_.contains(cachedOperationId));

    TryxRuntimeApplyRequest request;
    request.media = {QStringLiteral("existing.mp4.h264_2240x1080")};
    request.ratio = QStringLiteral("2:1");
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Single");
    QCOMPARE(manager->queueApplyOperation(cachedOperationId, request),
             cachedOperationId);
    QCOMPARE(manager->operationInfo(cachedOperationId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(cachedOperationId).errorCategory,
             QStringLiteral("RetryCacheValidationPending"));

    const QString blockedOperationId =
        QStringLiteral("88888888-8888-4888-8888-888888888888");
    QCOMPARE(manager->queueApplyOperation(blockedOperationId, request),
             blockedOperationId);
    QCOMPARE(manager->operationInfo(blockedOperationId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(blockedOperationId).errorCategory,
             QStringLiteral("RetryCacheValidationPending"));

    manager->handleRetryCacheValidation(cachedOperationId, true, false,
                                        QString());
    const QString recoveredOperationId = manager->retryCacheOperationId_;
    QVERIFY(!recoveredOperationId.isEmpty());
    QVERIFY(recoveredOperationId != cachedOperationId);
    QCOMPARE(manager->operationInfo(cachedOperationId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(recoveredOperationId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->retryCachePreparedPath_, preparedPath);
    QVERIFY(manager->pendingRetryValidationId_.isEmpty());
    QVERIFY(QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(
        QDir(manager->retryCacheDirectory()).filePath(
            QStringLiteral("orphan.h264"))));
}

void PrinterProtocolTests::retryV9PreservesApplyRequest() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));

    QString operationId =
        QStringLiteral("29292929-2929-4929-8929-292929292929");
    QJsonObject baseManifest;
    QString originalPreparedPath;
    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        const QByteArray preparedBytes(8192, '\x63');
        const QString preparedPath =
            QDir(cacheDirectory).filePath(
                QStringLiteral("apply-request.h264"));
        originalPreparedPath = preparedPath;
        QFile preparedFile(preparedPath);
        QVERIFY(preparedFile.open(
            QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(preparedFile.write(preparedBytes),
                 static_cast<qint64>(preparedBytes.size()));
        preparedFile.close();

        const QString sourcePath =
            QDir(temporaryDirectory.path()).filePath(
                QStringLiteral("source.mp4"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(
            QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(sourceFile.write(QByteArrayLiteral("source")),
                 6);
        sourceFile.close();

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("UploadAndApply");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.terminalOutcome = QStringLiteral("NotStarted");
        record.info.primaryErrorCategory =
            QStringLiteral("NotStarted");
        record.info.primaryErrorMessage =
            QStringLiteral("prepared for retry");
        record.info.retryMode = QStringLiteral("PreparedMedia");
        record.info.subject = QStringLiteral("source.mp4");
        record.info.resultName =
            QStringLiteral("apply-request.mp4.h264_2240x1080");
        record.info.total = preparedBytes.size();
        record.info.applyAfterUpload = true;
        record.sourcePath = sourcePath;
        record.sourceFingerprint = QString(64, QLatin1Char('a'));
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(
                preparedBytes, QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = record.info.resultName;
        record.originalRemoteName = record.remoteName;
        record.uploadDeviceIdentity = QStringLiteral("PASE-RETRY-001");
        record.uploadDeviceGeneration = 7;
        record.updateMetrics = true;
        record.applyRequest.ratio = QStringLiteral("2:1");
        record.applyRequest.screenMode =
            QStringLiteral("Full Screen");
        record.applyRequest.playMode = QStringLiteral("Single");
        record.applyRequest.sysinfoLabels = {
            QStringLiteral("CPU Power")};
        record.applyRequest.settingsBadges = {
            QStringLiteral("CPU Badge"),
            QStringLiteral("GPU Badge")};
        record.applyRequest.settingsPosition =
            QStringLiteral("Bottom");
        record.applyRequest.settingsColor =
            QStringLiteral("#ff0000");
        record.applyRequest.settingsAlign =
            QStringLiteral("Right");
        record.applyRequest.waterfallMode = true;
        record.applyRequest.replaceOverlay = true;
        record.applyRequest.display.brightnessPresent = true;
        record.applyRequest.display.brightness = 72;
        record.applyRequest.display.backlightPresent = true;
        record.applyRequest.display.backlightEnabled = false;
        record.applyRequest.display.orientationPresent = true;
        record.applyRequest.display.mirrorMode = true;
        record.applyRequest.display.waterfallMode = true;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId, QStringLiteral("NotStarted"),
                     &cacheError),
                 qPrintable(cacheError));

        QFile manifestFile(writer->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonDocument document =
            QJsonDocument::fromJson(manifestFile.readAll());
        QVERIFY(document.isObject());
        QCOMPARE(document.object()
                     .value(QStringLiteral("version"))
                     .toInt(),
                 9);
        QVERIFY(document.object()
                    .value(QStringLiteral("applyRequest"))
                    .isObject());
        baseManifest = document.object();
    }

    std::unique_ptr<DeviceManager> reader(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        reader.get(), &DeviceManager::requestValidatePrinterRetryCache,
        reader->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    reader->loadRetryCache();
    QCOMPARE(reader->pendingRetryValidationId_, operationId);
    reader->handleRetryCacheValidation(
        operationId, true, false, QString());
    const QString restoredId = reader->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    const TryxRuntimeApplyRequest restored =
        reader->operations_.value(restoredId).applyRequest;
    QCOMPARE(restored.sysinfoLabels,
             QStringList{QStringLiteral("CPU Power")});
    QCOMPARE(restored.settingsBadges,
             QStringList({QStringLiteral("CPU Badge"),
                          QStringLiteral("GPU Badge")}));
    QCOMPARE(restored.settingsPosition, QStringLiteral("Bottom"));
    QCOMPARE(restored.settingsColor, QStringLiteral("#ff0000"));
    QCOMPARE(restored.settingsAlign, QStringLiteral("Right"));
    QCOMPARE(restored.replaceOverlay, true);
    QCOMPARE(restored.display.brightnessPresent, true);
    QCOMPARE(restored.display.brightness, 72);
    QCOMPARE(restored.display.standbyPresent, false);
    QCOMPARE(restored.display.backlightPresent, true);
    QCOMPARE(restored.display.backlightEnabled, false);
    QCOMPARE(restored.display.orientationPresent, true);
    QCOMPARE(restored.display.mirrorMode, true);
    QCOMPARE(restored.display.waterfallMode, true);
    reader.reset();

    const auto writeManifestClone =
        [&baseManifest, &originalPreparedPath](
            DeviceManager *manager, const QString &directory,
            const QString &cloneOperationId,
            const std::function<void(QJsonObject *)> &mutate) {
            manager->retryCacheDirectoryOverride_ = directory;
            if (!QDir().mkpath(directory)) {
                return false;
            }
            QFile source(originalPreparedPath);
            if (!source.open(QIODevice::ReadOnly)) {
                return false;
            }
            const QString clonedPreparedPath =
                QDir(directory).filePath(
                    QStringLiteral("apply-request.h264"));
            if (!writeTextFile(clonedPreparedPath, source.readAll())) {
                return false;
            }
            QJsonObject manifest = baseManifest;
            manifest.insert(QStringLiteral("operationId"),
                            cloneOperationId);
            manifest.insert(QStringLiteral("preparedPath"),
                            clonedPreparedPath);
            manifest.insert(QStringLiteral("thumbnailStagingPath"),
                            QString());
            manifest.insert(QStringLiteral("thumbnailSize"), 0);
            manifest.insert(QStringLiteral("thumbnailSha256"),
                            QString());
            mutate(&manifest);
            return writeTextFile(
                manager->retryCacheManifestPath(),
                QJsonDocument(manifest).toJson(
                    QJsonDocument::Compact));
        };

    {
        std::unique_ptr<DeviceManager> strictReader(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString strictDirectory =
            QDir(temporaryDirectory.path()).filePath(
                QStringLiteral("retry-v9-missing-backlight"));
        const QString strictOperationId =
            QStringLiteral("29292929-2929-4929-8929-292929292930");
        QVERIFY(writeManifestClone(
            strictReader.get(), strictDirectory,
            strictOperationId,
            [](QJsonObject *manifest) {
                QJsonObject request =
                    manifest->value(
                        QStringLiteral("applyRequest")).toObject();
                QJsonObject display =
                    request.value(
                        QStringLiteral("display")).toObject();
                display.remove(
                    QStringLiteral("backlightEnabled"));
                request.insert(QStringLiteral("display"), display);
                manifest->insert(
                    QStringLiteral("applyRequest"), request);
            }));
        strictReader->loadRetryCache();
        QVERIFY(strictReader->pendingRetryValidationId_.isEmpty());
        QVERIFY(!QFileInfo::exists(
            strictReader->retryCacheManifestPath()));
    }

    {
        std::unique_ptr<DeviceManager> legacyReader(
            DeviceManager::createForTesting(sysRoot, devRoot));
        QObject::disconnect(
            legacyReader.get(),
            &DeviceManager::requestValidatePrinterRetryCache,
            legacyReader->printerMediaPreparer_,
            &PrinterMediaPreparer::validateRetryCache);
        const QString legacyDirectory =
            QDir(temporaryDirectory.path()).filePath(
                QStringLiteral("retry-v8-standby"));
        const QString legacyOperationId =
            QStringLiteral("29292929-2929-4929-8929-292929292931");
        QVERIFY(writeManifestClone(
            legacyReader.get(), legacyDirectory,
            legacyOperationId,
            [](QJsonObject *manifest) {
                manifest->insert(QStringLiteral("version"), 8);
                QJsonObject request =
                    manifest->value(
                        QStringLiteral("applyRequest")).toObject();
                QJsonObject display =
                    request.value(
                        QStringLiteral("display")).toObject();
                display.insert(
                    QStringLiteral("standbyPresent"), true);
                display.insert(
                    QStringLiteral("standbyEnabled"), false);
                display.remove(
                    QStringLiteral("backlightPresent"));
                display.remove(
                    QStringLiteral("backlightEnabled"));
                request.insert(QStringLiteral("display"), display);
                manifest->insert(
                    QStringLiteral("applyRequest"), request);
            }));
        legacyReader->loadRetryCache();
        QCOMPARE(legacyReader->pendingRetryValidationId_,
                 legacyOperationId);
        QVERIFY(!legacyReader->pendingRetryManifest_
                     .value(QStringLiteral("applyAfterUpload"))
                     .toBool(true));
        QVERIFY(!legacyReader->pendingRetryManifest_
                     .contains(QStringLiteral("applyRequest")));
        QVERIFY(!legacyReader->pendingRetryManifest_
                     .value(QStringLiteral("migrationDiagnostic"))
                     .toString().isEmpty());
        legacyReader->handleRetryCacheValidation(
            legacyOperationId, true, false, QString());
        const QString migratedId =
            legacyReader->retryCacheOperationId_;
        QVERIFY(!migratedId.isEmpty());
        const DeviceManager::OperationRecord migratedRecord =
            legacyReader->operations_.value(migratedId);
        QVERIFY(!migratedRecord.info.applyAfterUpload);
        QVERIFY(!migratedRecord.updateMetrics);
        QVERIFY(migratedRecord.info.message.contains(
            QStringLiteral("legacy PASE standby action")));
        QFile migratedFile(
            legacyReader->retryCacheManifestPath());
        QVERIFY(migratedFile.open(QIODevice::ReadOnly));
        const QJsonObject migratedManifest =
            QJsonDocument::fromJson(
                migratedFile.readAll()).object();
        QCOMPARE(migratedManifest
                     .value(QStringLiteral("version")).toInt(),
                 9);
        QVERIFY(!migratedManifest
                     .value(QStringLiteral("applyAfterUpload"))
                     .toBool(true));
        QVERIFY(!migratedManifest.contains(
            QStringLiteral("applyRequest")));
    }
}

void PrinterProtocolTests::ensureRetryManifestRequiresOriginIdentity() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("ensure.h264"));
    const QByteArray preparedBytes(4096, '\x37');
    QVERIFY(writeTextFile(preparedPath, preparedBytes));
    const QString preparedSha = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes, QCryptographicHash::Sha256)
            .toHex());
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), 6);
    manifest.insert(QStringLiteral("operationId"),
                    QStringLiteral("16161616-1616-4616-8616-161616161616"));
    manifest.insert(QStringLiteral("kind"),
                    QStringLiteral("EnsureMediaAndApply"));
    manifest.insert(QStringLiteral("applyAfterUpload"), true);
    manifest.insert(QStringLiteral("updateMetrics"), false);
    manifest.insert(QStringLiteral("attempt"), 1);
    manifest.insert(QStringLiteral("sourcePath"), QString());
    manifest.insert(QStringLiteral("sourceFingerprint"), QString());
    manifest.insert(QStringLiteral("preparedPath"), preparedPath);
    manifest.insert(QStringLiteral("preparedSize"), preparedBytes.size());
    manifest.insert(QStringLiteral("preparedSha256"), preparedSha);
    manifest.insert(QStringLiteral("remoteName"),
                    QStringLiteral("ensure.mp4.h264_2240x1080"));
    manifest.insert(QStringLiteral("terminalOutcome"),
                    QStringLiteral("PartialOrUnknown"));
    manifest.insert(QStringLiteral("requiresDeviceRecovery"), false);
    QVERIFY(writeTextFile(
        manager->retryCacheManifestPath(),
        QJsonDocument(manifest).toJson(QJsonDocument::Compact)));

    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    QSignalSpy validationSpy(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache);
    manager->loadRetryCache();
    QCOMPARE(validationSpy.count(), 0);
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(!QFileInfo::exists(preparedPath));
}

void PrinterProtocolTests::retryCacheHashValidationRejectsMismatch() {
    QTemporaryFile preparedFile;
    QVERIFY(preparedFile.open());
    QCOMPARE(preparedFile.write(QByteArrayLiteral("prepared-media")), 14);
    preparedFile.flush();

    PrinterMediaPreparer preparer;
    QSignalSpy validationSpy(&preparer,
                             &PrinterMediaPreparer::retryCacheValidated);
    const QString validationId =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    preparer.validateRetryCache(
        validationId, preparedFile.fileName(), QString(64, QLatin1Char('0')));
    QCOMPARE(validationSpy.count(), 1);
    QCOMPARE(validationSpy.first().at(0).toString(), validationId);
    QCOMPARE(validationSpy.first().at(1).toBool(), false);
    QCOMPARE(validationSpy.first().at(2).toBool(), false);
    QVERIFY(!validationSpy.first().at(3).toString().isEmpty());
}

void PrinterProtocolTests::retryCacheHashValidationHonorsPreStartCancellation() {
    QTemporaryFile preparedFile;
    QVERIFY(preparedFile.open());
    QCOMPARE(preparedFile.write(QByteArrayLiteral("prepared-media")), 14);
    preparedFile.flush();

    PrinterMediaPreparer preparer;
    QSignalSpy validationSpy(&preparer,
                             &PrinterMediaPreparer::retryCacheValidated);
    const QString validationId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    preparer.cancelRetryValidation(validationId);
    preparer.validateRetryCache(
        validationId, preparedFile.fileName(), QString(64, QLatin1Char('0')));
    QCOMPARE(validationSpy.count(), 1);
    QCOMPARE(validationSpy.first().at(0).toString(), validationId);
    QCOMPARE(validationSpy.first().at(1).toBool(), false);
    QCOMPARE(validationSpy.first().at(2).toBool(), true);
}

void PrinterProtocolTests::retryCancellationRetainsOwnershipOnManifestRemovalFailure() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QDir().mkpath(sysRoot);
    QDir().mkpath(devRoot);

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("retained.h264"));
    const QByteArray preparedBytes(4096, '\x41');
    QFile prepared(preparedPath);
    QVERIFY(prepared.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(prepared.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    prepared.close();

    const QString operationId =
        QStringLiteral("31313131-3131-4131-8131-313131313131");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("RetryAvailable");
    record.info.errorCategory = QStringLiteral("PartialOrUnknown");
    record.info.retryMode = QStringLiteral("PreparedMedia");
    record.info.total = preparedBytes.size();
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    record.remoteName =
        QStringLiteral("retained.mp4.h264_2240x1080");
    record.uploadDeviceIdentity =
        QStringLiteral("retained-test-pase");
    record.uploadDeviceGeneration = 1;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    QString cacheError;
    QVERIFY2(manager->writeRetryCache(
                 operationId, QStringLiteral("PartialOrUnknown"),
                 &cacheError),
             qPrintable(cacheError));

    QVERIFY(QFile::setPermissions(
        cacheDirectory,
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QSignalSpy errorSpy(manager.get(), &DeviceManager::deviceError);
    manager->cancelOperation(operationId);

    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("RetryCacheCleanupFailed"));
    QCOMPARE(manager->retryCacheOperationId_, operationId);
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(QFileInfo::exists(preparedPath));
    QVERIFY(errorSpy.count() >= 1);

    QVERIFY(QFile::setPermissions(
        cacheDirectory,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
}

void PrinterProtocolTests::mediaPreparationBoundsAndThumbnailFallback() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    {
        const QString oversizedPath = QDir(temporaryDirectory.path())
                                          .filePath(QStringLiteral(
                                              "oversized.h264"));
        QFile oversized(oversizedPath);
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(500LL * 1024LL * 1024LL + 1));
        oversized.close();

        PrinterMediaPreparer preparer;
        preparer.operationId_ =
            QStringLiteral("26262626-2626-4626-8626-262626262626");
        preparer.uploadPath_ = oversizedPath;
        preparer.generation_ = 31;
        preparer.active_ = true;
        preparer.phase_ = PrinterMediaPreparer::PreparationPhase::Media;
        QSignalSpy failedSpy(&preparer, &PrinterMediaPreparer::failed);
        preparer.finishMediaPreparation(0, true);
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(failedSpy.first().at(1).toString().contains(
            QStringLiteral("size"), Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(oversizedPath));
    }

    {
        const QString preparedPath = QDir(temporaryDirectory.path())
                                         .filePath(QStringLiteral(
                                             "prepared.h264"));
        const QString partialThumbnail = preparedPath +
            QStringLiteral(".part.jpg");
        QFile prepared(preparedPath);
        QVERIFY(prepared.open(QIODevice::WriteOnly));
        QCOMPARE(prepared.write(QByteArrayLiteral("valid-h264")), 10);
        prepared.close();
        QFile partial(partialThumbnail);
        QVERIFY(partial.open(QIODevice::WriteOnly));
        QCOMPARE(partial.write(QByteArrayLiteral("partial")), 7);
        partial.close();

        PrinterMediaPreparer preparer;
        preparer.operationId_ =
            QStringLiteral("27272727-2727-4727-8727-272727272727");
        preparer.devicePath_ = QStringLiteral("/dev/usb/lp-pase-preview");
        preparer.sourcePath_ = QStringLiteral("source.mp4");
        preparer.uploadPath_ = preparedPath;
        preparer.remoteName_ =
            QStringLiteral("prepared.mp4.h264_2240x1080");
        preparer.preparedSha256_ = QString(64, QLatin1Char('a'));
        preparer.stagedThumbnailTempPath_ = partialThumbnail;
        preparer.stagedThumbnailPath_ = preparedPath +
            QStringLiteral(".jpg");
        preparer.generation_ = 32;
        preparer.active_ = true;
        preparer.phase_ =
            PrinterMediaPreparer::PreparationPhase::Thumbnail;
        preparer.preparationTimedOut_ = true;
        QSignalSpy preparedSpy(&preparer, &PrinterMediaPreparer::prepared);
        QSignalSpy failedSpy(&preparer, &PrinterMediaPreparer::failed);
        preparer.finishThumbnailPreparation(-1, false);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(preparedSpy.count(), 1);
        QCOMPARE(preparedSpy.first().at(3).toString(), preparedPath);
        QVERIFY(preparedSpy.first().at(6).toString().isEmpty());
        QVERIFY(QFileInfo::exists(preparedPath));
        QVERIFY(!QFileInfo::exists(partialThumbnail));
    }

    {
        PrinterMediaPreparer preparer;
        const QString pendingId =
            QStringLiteral("28282828-2828-4828-8828-282828282828");
        preparer.active_ = true;
        preparer.operationId_ =
            QStringLiteral("29292929-2929-4929-8929-292929292929");
        preparer.hasPending_ = true;
        preparer.pendingOperationId_ = pendingId;
        preparer.pendingGeneration_ = 44;
        preparer.cancelOperation(pendingId);
        QVERIFY(!preparer.hasPending_);
        {
            QMutexLocker locker(&preparer.preparationCancellationMutex_);
            QVERIFY(!preparer.cancelledPreparationOperations_.contains(
                pendingId));
        }

        preparer.active_ = false;
        preparer.operationId_.clear();
        QSignalSpy failedSpy(&preparer, &PrinterMediaPreparer::failed);
        preparer.startPreparation(
            pendingId, QStringLiteral("/dev/usb/lp-pase-pending"),
            QDir(temporaryDirectory.path())
                .filePath(QStringLiteral("missing.mp4")),
            QString(), 44);
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(failedSpy.first().at(1).toString().contains(
            QStringLiteral("does not exist"), Qt::CaseInsensitive));
    }
}

void PrinterProtocolTests::shutdownPreservesPreparedMediaAsUnknownRetry() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath = QDir(cacheDirectory).filePath(
        QStringLiteral("shutdown-preserved.h264"));
    const QByteArray preparedBytes(16384, '\x53');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Transferring");
    record.info.stage = QStringLiteral("Transferring");
    record.info.subject = QStringLiteral("shutdown-source.mp4");
    record.info.resultName =
        QStringLiteral("shutdown-source.mp4.h264_2240x1080");
    record.info.total = preparedBytes.size();
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes, QCryptographicHash::Sha256)
            .toHex());
    record.remoteName = record.info.resultName;
    record.uploadDeviceIdentity =
        QStringLiteral("shutdown-test-pase");
    record.uploadDeviceGeneration = 1;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;
    manager->printerMediaPreparer_->deliveredPaths_.insert(preparedPath);

    manager->preserveActivePreparedMediaForShutdown();
    QCOMPARE(manager->retryCacheOperationId_, operationId);
    QCOMPARE(manager->retryCachePreparedPath_, preparedPath);
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(!manager->printerMediaPreparer_->deliveredPaths_.contains(
        preparedPath));

    manager.reset();
    QVERIFY(QFileInfo::exists(preparedPath));
    QFile manifestFile(QDir(cacheDirectory).filePath(
        QStringLiteral("retry-manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const QJsonDocument manifest = QJsonDocument::fromJson(
        manifestFile.readAll());
    QVERIFY(manifest.isObject());
    QCOMPARE(manifest.object()
                 .value(QStringLiteral("terminalOutcome"))
                 .toString(),
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manifest.object()
                .value(QStringLiteral("requiresDeviceRecovery"))
                .toBool());
}

void PrinterProtocolTests::postUploadRefreshFailureRemainsUnknownAndRetryable() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("refresh-failure.h264"));
    const QByteArray preparedBytes(2048, '\x62');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("66666666-6666-4666-8666-666666666666");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RefreshingMedia");
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes, QCryptographicHash::Sha256)
            .toHex());
    record.remoteName =
        QStringLiteral("verified-later.mp4.h264_2240x1080");
    record.originalRemoteName = record.remoteName;
    record.uploadDeviceIdentity =
        manager->printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration =
        manager->printerGeneration_;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    manager->worker_->printerMediaListFailed(
        operationId, QStringLiteral("refresh transport failed"),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QCOMPARE(manager->operationInfo(operationId).retryMode,
             QStringLiteral("PreparedMedia"));
    QVERIFY(QFileInfo::exists(preparedPath));
    QVERIFY(QFileInfo::exists(manager->retryCacheManifestPath()));

    manager->cancelOperation(operationId);
    QVERIFY(!QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
}

void PrinterProtocolTests::
    finalizationUnknownAutoReconcilesWithoutRetransmission() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(
            QStringLiteral("finalization-unknown.h264"));
    const QByteArray preparedBytes(8192, '\x6a');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                              QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("69696969-6969-4969-8969-696969696969");
    const QString remoteName =
        QStringLiteral("finalized.mp4.h264_2240x1080");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Ending");
    record.info.stage = QStringLiteral("Ending");
    record.info.subject = QStringLiteral("finalized.mp4");
    record.info.resultName = remoteName;
    record.info.total = preparedBytes.size();
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    record.remoteName = remoteName;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    QSignalSpy refreshSpy(
        manager.get(), &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy retransmitSpy(
        manager.get(), &DeviceManager::requestPrinterUploadPrepared);

    manager->worker_->printerUploadFinished(
        operationId, preparedPath, remoteName, false,
        PrinterProtocol::MutationOutcome::FinalizationUnknown,
        QStringLiteral("FileTransmitEnd status timed out"),
        manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Refreshing"));
    QCOMPARE(manager->operationInfo(operationId).stage,
             QStringLiteral("RecoveringFinalization"));
    QCOMPARE(retransmitSpy.count(), 0);
    QCOMPARE(refreshSpy.count(), 0);

    manager->worker_->printerSessionStarted(
        manager->printerGeneration_);
    QCOMPARE(refreshSpy.count(), 1);
    QCOMPARE(manager->operationInfo(operationId).stage,
             QStringLiteral("RefreshingMedia"));
    QCOMPARE(retransmitSpy.count(), 0);

    PrinterProtocol::MediaFile exact;
    exact.name = remoteName;
    exact.size = preparedBytes.size();
    exact.readOnly = false;
    exact.source = PrinterProtocol::MediaSource::User;
    manager->worker_->printerMediaListReady(
        operationId, {exact}, manager->printerGeneration_);

    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Succeeded"));
    QCOMPARE(retransmitSpy.count(), 0);
    QVERIFY(manager->activeOperationId_.isEmpty());
    QVERIFY(!QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
}

void PrinterProtocolTests::
    finalizationUnknownRebindsAfterUsbGenerationChange() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(
            QStringLiteral("generation-reconcile.h264"));
    const QByteArray preparedBytes(12288, '\x4c');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                              QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("6a6a6a6a-6a6a-4a6a-8a6a-6a6a6a6a6a6a");
    const QString remoteName =
        QStringLiteral("generation.mp4.h264_2240x1080");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Ending");
    record.info.stage = QStringLiteral("Ending");
    record.info.subject = QStringLiteral("generation.mp4");
    record.info.resultName = remoteName;
    record.info.total = preparedBytes.size();
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    record.remoteName = remoteName;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    QSignalSpy refreshSpy(
        manager.get(), &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy retransmitSpy(
        manager.get(), &DeviceManager::requestPrinterUploadPrepared);

    const quint64 uploadGeneration = manager->printerGeneration_;
    manager->worker_->printerUploadFinished(
        operationId, preparedPath, remoteName, false,
        PrinterProtocol::MutationOutcome::FinalizationUnknown,
        QStringLiteral("FileTransmitEnd status timed out"),
        uploadGeneration);
    QCOMPARE(manager->operationInfo(operationId).stage,
             QStringLiteral("RecoveringFinalization"));

    manager->cancelForegroundForGenerationChange(
        QStringLiteral("USB generation changed"));
    ++manager->printerGeneration_;
    const quint64 recoveryGeneration = manager->printerGeneration_;
    manager->worker_->printerSessionStarted(recoveryGeneration);

    QCOMPARE(refreshSpy.count(), 1);
    QCOMPARE(refreshSpy.first().at(2).toULongLong(),
             recoveryGeneration);
    QCOMPARE(manager->operations_.value(operationId)
                 .info.deviceGeneration,
             recoveryGeneration);
    QVERIFY(!manager->operations_.value(operationId)
                 .deviceChangePending);
    QCOMPARE(retransmitSpy.count(), 0);

    PrinterProtocol::MediaFile exact;
    exact.name = remoteName;
    exact.size = preparedBytes.size();
    exact.readOnly = false;
    exact.source = PrinterProtocol::MediaSource::User;
    manager->worker_->printerMediaListReady(
        operationId, {exact}, recoveryGeneration);

    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Succeeded"));
    QCOMPARE(retransmitSpy.count(), 0);
    QVERIFY(manager->activeOperationId_.isEmpty());
    QVERIFY(!QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
}

void PrinterProtocolTests::
    finalizationUnknownDoesNotAdoptReplacementDevice() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const QString originalIdentity = manager->printerDeviceSerial_;
    QVERIFY(!originalIdentity.isEmpty());
    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(
            QStringLiteral("replacement-device.h264"));
    const QByteArray preparedBytes(10240, '\x3e');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                              QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("6d6d6d6d-6d6d-4d6d-8d6d-6d6d6d6d6d6d");
    const QString remoteName =
        QStringLiteral("replacement.mp4.h264_2240x1080");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Ending");
    record.info.stage = QStringLiteral("Ending");
    record.info.subject = QStringLiteral("replacement.mp4");
    record.info.resultName = remoteName;
    record.info.total = preparedBytes.size();
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    record.remoteName = remoteName;
    record.uploadDeviceIdentity = originalIdentity;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    QSignalSpy refreshSpy(
        manager.get(), &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy retransmitSpy(
        manager.get(), &DeviceManager::requestPrinterUploadPrepared);

    const quint64 uploadGeneration = manager->printerGeneration_;
    manager->cancelForegroundForGenerationChange(
        QStringLiteral("USB device was replaced"));
    ++manager->printerGeneration_;
    manager->printerDeviceSerial_ = QStringLiteral("replacement-device");

    manager->worker_->printerUploadFinished(
        operationId, preparedPath, remoteName, false,
        PrinterProtocol::MutationOutcome::FinalizationUnknown,
        QStringLiteral("FileTransmitEnd status timed out"),
        uploadGeneration);
    QCOMPARE(manager->operations_.value(operationId)
                 .uploadDeviceIdentity,
             originalIdentity);

    manager->worker_->printerSessionStarted(
        manager->printerGeneration_);
    QCOMPARE(refreshSpy.count(), 0);
    QCOMPARE(retransmitSpy.count(), 0);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manager->operations_.value(operationId)
                .requiresDeviceRecovery);
    QVERIFY(manager->activeOperationId_.isEmpty());

    manager->cancelOperation(operationId);
    QVERIFY(!QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
}

void PrinterProtocolTests::
    finalizationUnknownRestartAutoReconcilesWithoutRetry() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString operationId =
        QStringLiteral("6b6b6b6b-6b6b-4b6b-8b6b-6b6b6b6b6b6b");
    const QString remoteName =
        QStringLiteral("restart.mp4.h264_2240x1080");
    QString preparedPath;
    const QByteArray preparedBytes(16384, '\x5d');

    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        preparedPath = QDir(cacheDirectory).filePath(
            QStringLiteral("restart-reconcile.h264"));
        QFile preparedFile(preparedPath);
        QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                                  QIODevice::Truncate));
        QCOMPARE(preparedFile.write(preparedBytes),
                 static_cast<qint64>(preparedBytes.size()));
        preparedFile.close();

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("Refreshing");
        record.info.stage =
            QStringLiteral("RecoveringFinalization");
        record.info.errorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.terminalOutcome =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorMessage =
            QStringLiteral("FileTransmitEnd status timed out");
        record.info.subject = QStringLiteral("restart.mp4");
        record.info.resultName = remoteName;
        record.info.total = preparedBytes.size();
        record.info.confirmedBytes = preparedBytes.size();
        record.info.lastConfirmedChunkIndex = 0;
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = remoteName;
        record.originalRemoteName = remoteName;
        record.uploadDeviceIdentity = QStringLiteral("1-1");
        record.uploadDeviceGeneration = 12;
        record.uploadFinalizationReconciliationPending = true;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId,
                     QStringLiteral("FinalizationUnknown"),
                     &cacheError),
                 qPrintable(cacheError));
    }

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    manager->loadRetryCache();
    const QString validationId = manager->pendingRetryValidationId_;
    QVERIFY(!validationId.isEmpty());
    QSignalSpy refreshSpy(
        manager.get(), &DeviceManager::requestPrinterRefreshMedia);
    QSignalSpy retransmitSpy(
        manager.get(), &DeviceManager::requestPrinterUploadPrepared);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    manager->handleRetryCacheValidation(validationId, true, false,
                                        QString());

    const QString restoredId = manager->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    QVERIFY(!manager->printerRecoveryRequired_);
    QVERIFY(manager->operations_.value(restoredId)
                .uploadFinalizationReconciliationPending);
    QCOMPARE(manager->activeOperationId_, restoredId);
    QCOMPARE(manager->operationInfo(restoredId).state,
             QStringLiteral("Refreshing"));
    QCOMPARE(manager->operationInfo(restoredId).stage,
             QStringLiteral("RecoveringFinalization"));
    QVERIFY(manager->operationInfo(restoredId).retryMode.isEmpty());
    QCOMPARE(manager->operationInfo(restoredId).errorCategory,
             QStringLiteral("FinalizationUnknown"));
    QCOMPARE(manager->operationInfo(restoredId).primaryErrorMessage,
             QStringLiteral("FileTransmitEnd status timed out"));
    QCOMPARE(manager->operationInfo(restoredId).confirmedBytes,
             preparedBytes.size());
    QCOMPARE(retransmitSpy.count(), 0);

    const QString retryId =
        QStringLiteral("6c6c6c6c-6c6c-4c6c-8c6c-6c6c6c6c6c6c");
    QCOMPARE(manager->retryOperation(restoredId, retryId), retryId);
    QCOMPARE(manager->operationInfo(retryId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(retryId).errorCategory,
             QStringLiteral("RetryUnavailable"));

    manager->worker_->printerSessionStarted(
        manager->printerGeneration_);
    QCOMPARE(refreshSpy.count(), 1);
    QCOMPARE(retransmitSpy.count(), 0);

    PrinterProtocol::MediaFile exact;
    exact.name = remoteName;
    exact.size = preparedBytes.size();
    exact.readOnly = false;
    exact.source = PrinterProtocol::MediaSource::User;
    manager->worker_->printerMediaListReady(
        restoredId, {exact}, manager->printerGeneration_);
    QCOMPARE(manager->operationInfo(restoredId).state,
             QStringLiteral("Succeeded"));
    QCOMPARE(retransmitSpy.count(), 0);
    QVERIFY(!QFileInfo::exists(preparedPath));
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
}

void PrinterProtocolTests::
    finalizationReconciliationMissSurvivesRestartAsFreshNameRetry() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString usbName = QStringLiteral("1-1");
    const QString lpName = QStringLiteral("lp0");
    const QString operationId =
        QStringLiteral("6d6d6d6d-6d6d-4d6d-8d6d-6d6d6d6d6d6d");
    const QString remoteName =
        QStringLiteral("missing.mp4.h264_2240x1080");
    const QByteArray preparedBytes(16384, '\x6d');
    QString preparedPath;

    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));
    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        preparedPath = QDir(cacheDirectory).filePath(
            QStringLiteral("missing-reconcile.h264"));
        QVERIFY(writeTextFile(preparedPath, preparedBytes));

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("Refreshing");
        record.info.stage =
            QStringLiteral("RecoveringFinalization");
        record.info.errorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.terminalOutcome =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorCategory =
            QStringLiteral("FinalizationUnknown");
        record.info.primaryErrorMessage =
            QStringLiteral("FileTransmitEnd status timed out");
        record.info.subject = QStringLiteral("missing.mp4");
        record.info.resultName = remoteName;
        record.info.total = preparedBytes.size();
        record.info.confirmedBytes = preparedBytes.size();
        record.info.lastConfirmedChunkIndex = 0;
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = remoteName;
        record.originalRemoteName = remoteName;
        record.uploadDeviceIdentity = usbName;
        record.uploadDeviceGeneration = 14;
        record.uploadFinalizationReconciliationPending = true;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId,
                     QStringLiteral("FinalizationUnknown"),
                     &cacheError),
                 qPrintable(cacheError));
    }

    {
        std::unique_ptr<DeviceManager> manager(
            DeviceManager::createForTesting(sysRoot, devRoot));
        QObject::disconnect(
            manager.get(),
            &DeviceManager::requestValidatePrinterRetryCache,
            manager->printerMediaPreparer_,
            &PrinterMediaPreparer::validateRetryCache);
        manager->loadRetryCache();
        const QString validationId =
            manager->pendingRetryValidationId_;
        QVERIFY(!validationId.isEmpty());
        QSignalSpy refreshSpy(
            manager.get(),
            &DeviceManager::requestPrinterRefreshMedia);
        manager->setAutoConnectModeForTesting(true);
        manager->rescanPrinterForTesting();
        manager->handleRetryCacheValidation(
            validationId, true, false, QString());
        const QString restoredId =
            manager->retryCacheOperationId_;
        manager->worker_->printerSessionStarted(
            manager->printerGeneration_);
        QCOMPARE(refreshSpy.count(), 1);
        manager->worker_->printerMediaListReady(
            restoredId, {}, manager->printerGeneration_);

        QCOMPARE(manager->operationInfo(restoredId).state,
                 QStringLiteral("RetryAvailable"));
        QCOMPARE(manager->operationInfo(restoredId).terminalOutcome,
                 QStringLiteral("PartialOrUnknown"));
        QCOMPARE(
            manager->operationInfo(restoredId).primaryErrorCategory,
            QStringLiteral("FinalizationUnknown"));
        QVERIFY(manager->operations_.value(restoredId)
                    .retryMustUseNewRemoteName);
        QVERIFY(manager->operations_.value(restoredId)
                    .requiresDeviceRecovery);
        QVERIFY(manager->printerRecoveryRequired_);

        QFile manifestFile(manager->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        QCOMPARE(
            manifest.value(QStringLiteral("terminalOutcome"))
                .toString(),
            QStringLiteral("PartialOrUnknown"));
        QVERIFY(manifest
                    .value(QStringLiteral("requiresNewRemoteName"))
                    .toBool());
        QVERIFY(!manifest
                     .value(QStringLiteral(
                         "finalizationOnlyReconciliation"))
                     .toBool());
    }

    std::unique_ptr<DeviceManager> restarted(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        restarted.get(),
        &DeviceManager::requestValidatePrinterRetryCache,
        restarted->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    restarted->loadRetryCache();
    const QString restartValidationId =
        restarted->pendingRetryValidationId_;
    QVERIFY(!restartValidationId.isEmpty());
    restarted->setAutoConnectModeForTesting(true);
    restarted->rescanPrinterForTesting();
    restarted->handleRetryCacheValidation(
        restartValidationId, true, false, QString());
    const QString restoredId = restarted->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    QCOMPARE(restarted->operationInfo(restoredId).terminalOutcome,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(restarted->operations_.value(restoredId)
                .retryMustUseNewRemoteName);
    QVERIFY(restarted->printerRecoveryRequired_);
    QVERIFY(QFileInfo::exists(preparedPath));

    const QString usbDevicePath =
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/") +
                               usbName);
    const QString classPath =
        QDir(sysRoot).filePath(QStringLiteral("class/usbmisc/") +
                               lpName);
    QVERIFY(QDir(usbDevicePath).removeRecursively());
    QVERIFY(QDir(classPath).removeRecursively());
    QVERIFY(QFile::remove(
        QDir(devRoot).filePath(QStringLiteral("usb/") + lpName)));
    restarted->rescanPrinterForTesting();
    QVERIFY(restarted->printerRecoveryRemovalObserved_);
    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));
    restarted->rescanPrinterForTesting();
    QVERIFY(!restarted->printerRecoveryRequired_);
    restarted->printerDisplaySessionActive_ = true;

    QSignalSpy uploadSpy(
        restarted.get(),
        &DeviceManager::requestPrinterUploadPrepared);
    const QString retryId =
        QStringLiteral("6e6e6e6e-6e6e-4e6e-8e6e-6e6e6e6e6e6e");
    QCOMPARE(restarted->retryOperation(restoredId, retryId), retryId);
    restarted->printerMediaPreparer_->retryCacheValidated(
        retryId, true, false, QString());
    PrinterProtocol::MediaFile uncertain;
    uncertain.name = remoteName;
    uncertain.size = preparedBytes.size();
    uncertain.source = PrinterProtocol::MediaSource::User;
    restarted->worker_->printerMediaListReady(
        retryId, {uncertain}, restarted->printerGeneration_);
    QCOMPARE(uploadSpy.count(), 1);
    QVERIFY(uploadSpy.first().at(2).toString() != remoteName);
}

void PrinterProtocolTests::
applyPreflightTimeoutPreservesNotStartedBeforeSessionLoss() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString usbName = QStringLiteral("1-1");
    const QString lpName = QStringLiteral("lp0");
    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(
        sysRoot, devRoot, usbName, lpName));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));
    const QString endpoint =
        QDir(devRoot).filePath(
            QStringLiteral("usb/") + lpName);
    DeviceWorker *worker = manager->worker_;
    QVERIFY(QMetaObject::invokeMethod(
        worker,
        [worker, fd = sockets[0], endpoint]() {
            worker->printerProtocol_ =
                std::make_unique<PrinterProtocol>(75);
            worker->adoptPrinterFileDescriptorForTesting(
                fd, endpoint);
            worker->printerSessionState_ =
                DeviceWorker::PrinterSessionState::Active;
        },
        Qt::BlockingQueuedConnection));
    manager->printerDisplaySessionActive_ = true;
    manager->printerDisplaySessionLost_ = false;

    int queryCount = 0;
    quint64 firstTrackId = 0;
    quint64 secondTrackId = 0;
    QString peerError;
    std::thread peer([&]() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            panorama::wire::v1::Request request;
            if (!readRequest(
                    sockets[1], &request, &peerError) ||
                request.body_case() !=
                    panorama::wire::v1::Request::
                        kUserConfigurationQuery ||
                !request.has_header() ||
                request.header().track_id() == 0) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "missing manager apply preflight query %1")
                                    .arg(attempt + 1);
                }
                return;
            }
            ++queryCount;
            if (attempt == 0) {
                firstTrackId = request.header().track_id();
            } else {
                secondTrackId = request.header().track_id();
            }
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult =
            ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = pollResult == 0
                ? QStringLiteral(
                      "manager apply transport remained open after timeout")
                : QStringLiteral(
                      "failed to inspect manager apply transport");
            return;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            const ssize_t received =
                ::recv(
                    sockets[1], &byte, sizeof(byte),
                    MSG_DONTWAIT | MSG_PEEK);
            if (received > 0) {
                peerError = QStringLiteral(
                    "manager apply sent a mutation after preflight timeout");
            }
        }
    });

    TryxRuntimeApplyRequest request;
    request.media = {
        QStringLiteral("left.mp4.h264_2240x1080"),
        QStringLiteral("right.mp4.h264_2240x1080")};
    request.ratio = QStringLiteral("2:1");
    request.screenMode =
        QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    const QString operationId =
        QStringLiteral(
            "76767676-7676-4676-8676-767676767676");
    const QString queuedOperationId =
        manager->queueApplyOperation(operationId, request);

    QElapsedTimer terminalTimer;
    terminalTimer.start();
    while ((manager->operationInfo(operationId).state !=
                QStringLiteral("Failed") ||
            !manager->printerDisplaySessionLost_) &&
           terminalTimer.elapsed() < kPeerTimeoutMs * 2) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 10);
    }

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(queuedOperationId, operationId);
    QCOMPARE(queryCount, 2);
    QVERIFY(firstTrackId != 0);
    QVERIFY(secondTrackId != 0);
    QVERIFY(firstTrackId != secondTrackId);
    const TryxRuntimeOperationInfo info =
        manager->operationInfo(operationId);
    QCOMPARE(info.state, QStringLiteral("Failed"));
    QCOMPARE(info.errorCategory,
             QStringLiteral("NotStarted"));
    QVERIFY(info.retryMode.isEmpty());
    QVERIFY(manager->activeOperationInfo().id.isEmpty());
    QVERIFY(!manager->operations_.value(operationId)
                 .deviceChangePending);
    QVERIFY(manager->printerDisplaySessionLost_);
}

void PrinterProtocolTests::generationChangeWaitsForStructuredApplyOutcome() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const quint64 oldGeneration = manager->printerGeneration_;
    const QString operationId =
        QStringLiteral("77777777-7777-4777-8777-777777777777");
    const QString mediaName =
        QStringLiteral("existing.mp4.h264_2240x1080");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Apply");
    record.info.state = QStringLiteral("Applying");
    record.info.stage = QStringLiteral("Applying");
    record.info.deviceGeneration = oldGeneration;
    record.mediaFile = mediaName;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;

    manager->cancelForegroundForGenerationChange(
        QStringLiteral("USB generation changed"));
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Applying"));
    QCOMPARE(manager->activeOperationInfo().id, operationId);
    QVERIFY(manager->operations_.value(operationId).deviceChangePending);

    ++manager->printerGeneration_;
    manager->worker_->printerApplyFinished(
        operationId, mediaName, false, false,
        PrinterProtocol::MutationOutcome::PartialOrUnknown,
        QStringLiteral("activation outcome unknown"), oldGeneration);
    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QCOMPARE(manager->operationInfo(operationId).retryMode,
             QStringLiteral("ReconcileOnly"));
    QVERIFY(manager->activeOperationInfo().id.isEmpty());
}

void PrinterProtocolTests::wireCriticalGoldenFixtures() {
    const auto serializedHex = [](const auto &message) {
        return QByteArray::fromStdString(message.SerializeAsString()).toHex();
    };

    panorama::wire::v1::Request bootstrap;
    bootstrap.mutable_header()->set_version(1);
    bootstrap.mutable_device_information_query()->set_dummy("NA");
    QCOMPARE(serializedHex(bootstrap),
             QByteArray("0a020801a206040a024e41"));

    panorama::wire::v1::Request ping;
    ping.mutable_header()->set_version(1);
    ping.mutable_header()->set_track_id(42);
    ping.mutable_ping()->set_payload("hello?");
    QCOMPARE(serializedHex(ping),
             QByteArray("0a040801102a52080a0668656c6c6f3f"));

    panorama::wire::v1::Request removal;
    removal.mutable_header()->set_version(1);
    removal.mutable_header()->set_track_id(42);
    removal.mutable_file_removal()->set_file_name("clip.mp4");
    removal.mutable_file_removal()->set_file_type("media");
    QCOMPARE(
        serializedHex(removal),
        QByteArray("0a040801102a9a19110a08636c69702e6d703412056d65646961"));

    panorama::wire::v1::Request transferBegin;
    transferBegin.mutable_header()->set_track_id(17);
    transferBegin.mutable_transfer_begin()->set_file_name("clip.mp4");
    transferBegin.mutable_transfer_begin()->set_file_size(123456);
    QCOMPARE(
        serializedHex(transferBegin),
        QByteArray("0a02101182190e0a08636c69702e6d703410c0c407"));

    panorama::wire::v1::Request transferData;
    transferData.mutable_header()->set_track_id(17);
    transferData.mutable_transfer_chunk()->set_file_data("abc");
    QCOMPARE(serializedHex(transferData),
             QByteArray("0a0210118a19050a03616263"));

    panorama::wire::v1::Request transferEnd;
    transferEnd.mutable_header()->set_track_id(17);
    transferEnd.mutable_transfer_end()->set_file_type("media");
    transferEnd.mutable_transfer_end()->set_checksum(0);
    QCOMPARE(serializedHex(transferEnd),
             QByteArray("0a0210119219070a056d65646961"));

    panorama::wire::v1::Request metricBatch;
    auto *metricGroup =
        metricBatch.mutable_metric_batch()->add_label_groups();
    metricGroup->set_group_id(100);
    auto *metricLabel = metricGroup->add_label_texts();
    metricLabel->set_label_id(101);
    metricLabel->set_text("42");
    QCOMPARE(serializedHex(metricBatch),
             QByteArray("e2120c0a0a08641206086512023432"));

    panorama::wire::v1::Request layout;
    layout.mutable_header();
    auto *layoutGroup = layout.mutable_overlay_layout()->add_label_groups();
    layoutGroup->set_group_id(100);
    layoutGroup->set_group_x(60);
    layoutGroup->set_group_y(100);
    layoutGroup->set_group_width(1000);
    layoutGroup->set_group_height(160);
    layoutGroup->set_text_align(
        panorama::wire::v1::OverlayGroup::ALIGN_LEFT);
    layoutGroup->set_line_gap(-10);
    auto *layoutLabel = layoutGroup->add_labels();
    layoutLabel->set_label_id(101);
    layoutLabel->set_line(1);
    layoutLabel->set_gap_left(-3);
    layoutLabel->set_text_font("roboto-regular");
    layoutLabel->set_text_size(30);
    layoutLabel->set_text_color(14474460);
    layoutLabel->set_text("CPU");
    QCOMPARE(
        serializedHex(layout),
        QByteArray(
            "0a00ca0c360a340864103c186420e80728a001300138134222086510012005"
            "420e726f626f746f2d726567756c6172481e50dcb9f3065a03435055"));

    panorama::wire::v1::Request userConfiguration;
    userConfiguration.mutable_header()->set_version(1);
    userConfiguration.mutable_header()->set_track_id(7);
    auto *configuration = userConfiguration.mutable_user_configuration();
    configuration->mutable_standby_config()->set_enable(true);
    configuration->mutable_standby_config()->set_media_file("standby.h264");
    configuration->mutable_work_config()->set_media_mode(
        panorama::wire::v1::WorkConfiguration::MEDIA_DUAL);
    configuration->mutable_work_config()->set_loop_mode(
        panorama::wire::v1::WorkConfiguration::LOOP_RANDOM);
    configuration->mutable_work_config()->set_dual_mode_left_media_file(
        "left.h264");
    configuration->mutable_work_config()->set_dual_mode_right_media_file(
        "right.h264");
    configuration->mutable_display_config()->set_backlight_enable(true);
    configuration->mutable_display_config()->set_backlight_brightness(77);
    configuration->mutable_display_config()->set_mirror(true);
    configuration->mutable_display_config()->set_ui_rotation(90);
    configuration->mutable_display_config()->set_media_rotation(180);
    QCOMPARE(
        serializedHex(userConfiguration),
        QByteArray(
            "0a0408011007c20c3c12100801120c7374616e6462792e683236341a1b080110"
            "0222096c6566742e683236342a0a72696768742e683236342a0b0801104d1801"
            "205a28b401"));

    panorama::wire::v1::Response mediaCatalog;
    mediaCatalog.mutable_header()->set_version(1);
    mediaCatalog.mutable_header()->set_track_id(8);
    auto *userMedia = mediaCatalog.mutable_media_catalog()->add_media_file_list();
    userMedia->set_file_path("/userdata/a");
    userMedia->set_file_ext(".mp4");
    userMedia->set_file_size(123);
    auto *presetMedia =
        mediaCatalog.mutable_media_catalog()->add_preset_file_list();
    presetMedia->set_file_path("default_x");
    presetMedia->set_file_ext(".mp4");
    presetMedia->set_file_size(456);
    presetMedia->set_read_only(true);
    QCOMPARE(
        serializedHex(mediaCatalog),
        QByteArray(
            "0a0408011008ba1f2f0a150a0b2f75736572646174612f6112042e6d703418"
            "7b12160a0964656661756c745f7812042e6d703418c8032001"));

    panorama::wire::v1::Response error;
    error.mutable_header()->set_version(1);
    error.mutable_header()->set_track_id(9);
    error.mutable_error()->set_code(
        panorama::wire::v1::ProtocolError::FAILURE);
    error.mutable_error()->set_why("rejected");
    QCOMPARE(serializedHex(error),
             QByteArray("0a0408011009120c0801120872656a6563746564"));

    panorama::wire::v1::Response transferStatus;
    transferStatus.mutable_header()->set_track_id(17);
    transferStatus.mutable_transfer_end_status()->set_status(
        panorama::wire::v1::TransferStatus::CHECKSUM_FAILURE);
    QCOMPARE(serializedHex(transferStatus),
             QByteArray("0a0210119232020803"));

    panorama::wire::v1::Response asynchronousEvent;
    asynchronousEvent.mutable_asynchronous_event()->set_play_finished(true);
    QCOMPARE(serializedHex(asynchronousEvent),
             QByteArray("da3d020801"));
}

void PrinterProtocolTests::unknownFieldsSurviveMutation() {
    panorama::wire::v1::UserConfiguration original;
    original.mutable_work_config()->set_single_mode_media_file("old.h264");
    original.GetReflection()->MutableUnknownFields(&original)->AddVarint(99, 123456);
    auto *workUnknown = original.mutable_work_config()
                            ->GetReflection()
                            ->MutableUnknownFields(original.mutable_work_config());
    workUnknown->AddLengthDelimited(77, "nested-unknown");

    std::string fixture;
    QVERIFY(original.SerializeToString(&fixture));
    panorama::wire::v1::UserConfiguration parsed;
    QVERIFY(parsed.ParseFromString(fixture));
    parsed.mutable_work_config()->set_single_mode_media_file("new.h264");

    std::string roundTrip;
    QVERIFY(parsed.SerializeToString(&roundTrip));
    panorama::wire::v1::UserConfiguration verified;
    QVERIFY(verified.ParseFromString(roundTrip));
    QCOMPARE(QString::fromStdString(
                 verified.work_config().single_mode_media_file()),
             QStringLiteral("new.h264"));
    QVERIFY(hasUnknownField(
        verified.GetReflection()->GetUnknownFields(verified), 99));
    QVERIFY(hasUnknownField(
        verified.work_config().GetReflection()->GetUnknownFields(
            verified.work_config()),
        77));
}

void PrinterProtocolTests::discoveryStateSequence() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString sysRoot = QDir(fixture.path()).filePath(QStringLiteral("sys"));
    const QString devRoot = QDir(fixture.path()).filePath(QStringLiteral("dev"));

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "0006"));
    auto snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::RockchipGadget391a0006);
    QVERIFY(snapshot.blocksLegacyTransport());

    QVERIFY(writeTextFile(
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-1/idProduct")),
        "1021\n"));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Enumerating391a1021);

    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"), QStringLiteral("lp0")));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Ready);
    QCOMPARE(snapshot.devices.size(), 1);
    QVERIFY(snapshot.devices.first().accessible);

    QVERIFY(writeTextFile(
        QDir(sysRoot).filePath(
            QStringLiteral("bus/usb/devices/1-1/1-1:1.0/bInterfaceProtocol")),
        "01\n"));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Enumerating391a1021);
    QVERIFY(writeTextFile(
        QDir(sysRoot).filePath(
            QStringLiteral("bus/usb/devices/1-1/1-1:1.0/bInterfaceProtocol")),
        "02\n"));

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-2"), "0006"));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Ambiguous);
    QVERIFY(QDir(QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-2")))
                .removeRecursively());

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-2"), "1021"));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Ambiguous);
    QCOMPARE(snapshot.devices.size(), 1);

    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-2"), QStringLiteral("lp1")));
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Ambiguous);
    QCOMPARE(snapshot.devices.size(), 2);

    QVERIFY(QDir(QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-1")))
                .removeRecursively());
    QVERIFY(QDir(QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-2")))
                .removeRecursively());
    QVERIFY(QDir(QDir(sysRoot).filePath(QStringLiteral("class/usbmisc")))
                .removeRecursively());
    snapshot = PrinterProtocol::discover(sysRoot, devRoot);
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::Absent);
    QVERIFY(!snapshot.blocksLegacyTransport());
}

void PrinterProtocolTests::productionEndpointValidationWithOfflineSysfs() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"), QStringLiteral("lp0")));

    const QString endpointPath = QDir(devRoot).filePath(QStringLiteral("usb/lp0"));
    QVERIFY(QFile::remove(endpointPath));
    const QByteArray encodedEndpoint = QFile::encodeName(endpointPath);
    QVERIFY(::symlink("/dev/null", encodedEndpoint.constData()) == 0);

    struct stat nullStatus {};
    QVERIFY(::stat("/dev/null", &nullStatus) == 0);
    QVERIFY(S_ISCHR(nullStatus.st_mode));
    const QString sysfsDevPath = QDir(sysRoot).filePath(
        QStringLiteral("class/usbmisc/lp0/dev"));
    const QByteArray expectedDeviceNumber =
        QByteArray::number(major(nullStatus.st_rdev)) + ':' +
        QByteArray::number(minor(nullStatus.st_rdev)) + '\n';
    QVERIFY(writeTextFile(sysfsDevPath, expectedDeviceNumber));

    const int endpointFd = ::open(encodedEndpoint.constData(),
                                  O_RDWR | O_CLOEXEC | O_NONBLOCK);
    QVERIFY(endpointFd >= 0);
    QString error;
    QVERIFY2(PrinterProtocol::validateEndpointForTesting(
                 endpointPath, endpointFd, sysRoot, devRoot, &error),
             qPrintable(error));

    const QString productPath = QDir(sysRoot).filePath(
        QStringLiteral("bus/usb/devices/1-1/idProduct"));
    QVERIFY(writeTextFile(productPath, "0006\n"));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        endpointPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("391a:1021")));
    QVERIFY(writeTextFile(productPath, "1021\n"));

    const QString protocolPath = QDir(sysRoot).filePath(
        QStringLiteral("bus/usb/devices/1-1/1-1:1.0/bInterfaceProtocol"));
    QVERIFY(writeTextFile(protocolPath, "01\n"));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        endpointPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("printer interface")));
    QVERIFY(writeTextFile(protocolPath, "02\n"));

    QVERIFY(writeTextFile(sysfsDevPath, "1:1\n"));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        endpointPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("sysfs")));
    QVERIFY(writeTextFile(sysfsDevPath, expectedDeviceNumber));

    QVERIFY(QFile::remove(endpointPath));
    QVERIFY(::symlink("/dev/zero", encodedEndpoint.constData()) == 0);
    struct stat zeroStatus {};
    QVERIFY(::stat("/dev/zero", &zeroStatus) == 0);
    const QByteArray replacementDeviceNumber =
        QByteArray::number(major(zeroStatus.st_rdev)) + ':' +
        QByteArray::number(minor(zeroStatus.st_rdev)) + '\n';
    QVERIFY(writeTextFile(sysfsDevPath, replacementDeviceNumber));
    QVERIFY2(PrinterProtocol::validateEndpointForTesting(
                 endpointPath, -1, sysRoot, devRoot, &error),
             qPrintable(error));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        endpointPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("changed")));

    const QString unverifiedPath = QDir(devRoot).filePath(QStringLiteral("lp0"));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        unverifiedPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("unverified")));
    const QString signedEndpointPath = QDir(devRoot).filePath(
        QStringLiteral("usb/lp+1"));
    QVERIFY(!PrinterProtocol::validateEndpointForTesting(
        signedEndpointPath, endpointFd, sysRoot, devRoot, &error));
    QVERIFY(error.contains(QStringLiteral("unverified")));
    ::close(endpointFd);
}

void PrinterProtocolTests::samePathEndpointEventForcesNewEpochSignal() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"), QStringLiteral("lp0")));

    PrinterDeviceMonitor monitor;
    monitor.setDiscoveryRootsForTesting(sysRoot, devRoot);
    QSignalSpy snapshotSpy(&monitor, &PrinterDeviceMonitor::snapshotChanged);
    monitor.rescanForTesting(false);
    QCOMPARE(snapshotSpy.count(), 1);
    const PrinterProtocol::DiscoverySnapshot first = monitor.snapshot();
    QVERIFY(first.state == PrinterProtocol::DiscoveryState::Ready);
    QCOMPARE(first.devices.size(), 1);
    QCOMPARE(first.devices.first().devicePath,
             QDir(devRoot).filePath(QStringLiteral("usb/lp0")));

    monitor.rescanForTesting(false);
    QCOMPARE(snapshotSpy.count(), 1);
    monitor.injectUdevEventForTesting(
        QByteArrayLiteral("usb"),
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-1")),
        QStringLiteral("1-1"));
    QCOMPARE(snapshotSpy.count(), 2);
    QVERIFY(monitor.snapshot() == first);
}

void PrinterProtocolTests::paseUdevReadinessUsesUsbDeviceEvents() {
    const auto usbAdd = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usb"), QByteArrayLiteral("add"),
        QByteArrayLiteral("391a/1021/100"));
    QCOMPARE(usbAdd, qMakePair(true, false));

    const auto usbBind = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usb"), QByteArrayLiteral("bind"),
        QByteArrayLiteral("391A/1021/100"));
    QCOMPARE(usbBind, qMakePair(false, false));

    const auto usbmiscAdd = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usbmisc"), QByteArrayLiteral("add"), QByteArray());
    QCOMPARE(usbmiscAdd, qMakePair(false, false));

    const auto usbmiscRemove = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usbmisc"), QByteArrayLiteral("remove"), QByteArray());
    QCOMPARE(usbmiscRemove, qMakePair(false, false));

    const auto usbRemove = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usb"), QByteArrayLiteral("remove"),
        QByteArrayLiteral("391a/1021/100"), true);
    QCOMPARE(usbRemove, qMakePair(true, true));

    const auto unrelatedRemove =
        PrinterDeviceMonitor::eventPolicyForTesting(
            QByteArrayLiteral("usb"), QByteArrayLiteral("remove"),
            QByteArrayLiteral("0db0/84df/0"));
    QCOMPARE(unrelatedRemove, qMakePair(false, false));

    const auto unknownUnrelatedRemove =
        PrinterDeviceMonitor::eventPolicyForTesting(
            QByteArrayLiteral("usb"), QByteArrayLiteral("remove"),
            QByteArray());
    QCOMPARE(unknownUnrelatedRemove, qMakePair(false, false));

    const auto currentPaseRemoveWithoutProduct =
        PrinterDeviceMonitor::eventPolicyForTesting(
            QByteArrayLiteral("usb"), QByteArrayLiteral("remove"),
            QByteArray(), true);
    QCOMPARE(currentPaseRemoveWithoutProduct,
             qMakePair(true, true));

    const auto transitionAdd = PrinterDeviceMonitor::eventPolicyForTesting(
        QByteArrayLiteral("usb"), QByteArrayLiteral("add"),
        QByteArrayLiteral("391a/0006/100"));
    QCOMPARE(transitionAdd, qMakePair(true, false));
}

void PrinterProtocolTests::unrelatedUsbRemoveDoesNotRestartPaseSession() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    const quint64 generation =
        manager->printerGenerationForTesting();
    manager->printerMonitor_->snapshot_.devices.first().sysfsPath =
        QDir(sysRoot).filePath(
            QStringLiteral("bus/usb/devices/1-1"));
    QSignalSpy sessionStartSpy(
        manager.get(), &DeviceManager::requestStartPrinterSession);

    manager->injectPrinterUdevEventForTesting(
        QByteArrayLiteral("usb"),
        QDir(sysRoot).filePath(
            QStringLiteral("bus/usb/devices/1-10.4")),
        QStringLiteral("1-10.4"));

    QCOMPARE(manager->printerGenerationForTesting(), generation);
    QCOMPARE(sessionStartSpy.count(), 0);
}

void PrinterProtocolTests::passivePrinterReconnectDoesNotRequestSessionResume() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    QSignalSpy resumeSpy(manager.get(),
                         &DeviceManager::requestStartPrinterSession);

    manager->injectPrinterUdevEventForTesting(
        QByteArrayLiteral("usb"),
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-1")),
        QStringLiteral("1-1"));
    QCOMPARE(resumeSpy.count(), 1);
    QCOMPARE(resumeSpy.first().at(1).toULongLong(),
             manager->printerGenerationForTesting());
}

void PrinterProtocolTests::samePathReenumerationCancelsOldGeneration() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot = QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"), QStringLiteral("lp0")));
    const QString endpointPath = QDir(devRoot).filePath(QStringLiteral("usb/lp0"));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QSignalSpy connectedSpy(manager.get(), &DeviceManager::deviceConnected);
    QSignalSpy rawFailureSpy(
        manager.get(), &DeviceManager::printerWorkerDeviceInfoFailedForTesting);
    QSignalSpy deliveredFailureSpy(
        manager.get(), &DeviceManager::printerDeviceInfoFailed);
    QSignalSpy operationsCancelledSpy(
        manager.get(), &DeviceManager::printerOperationsCancelled);
    QSignalSpy resumeSpy(manager.get(),
                         &DeviceManager::requestStartPrinterSession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QCOMPARE(connectedSpy.count(), 1);
    QVERIFY(manager->isPrinterClassConnected());
    const quint64 oldGeneration = manager->printerGenerationForTesting();
    QCOMPARE(operationsCancelledSpy.count(), 0);

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    QVERIFY(manager->adoptPrinterFileDescriptorForTesting(sockets[0], endpointPath));

    std::atomic_bool requestSeen{false};
    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(sockets[1], &peerError,
                               &requestBuffer)) {
            return;
        }
        panorama::wire::v1::Request sessionRequest;
        if (!readRequest(sockets[1], &sessionRequest, &peerError,
                         kPeerTimeoutMs, &requestBuffer) ||
            sessionRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral("same-path test did not receive session start");
            return;
        }
        auto sessionResponse = baseResponse(sessionRequest);
        sessionResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], sessionResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request request;
        bool deviceInfoReceived = false;
        for (int index = 0; index < 8; ++index) {
            if (!readRequest(sockets[1], &request, &peerError,
                             kPeerTimeoutMs * 3, &requestBuffer)) {
                return;
            }
            if (request.body_case() ==
                panorama::wire::v1::Request::kDeviceInformationQuery) {
                deviceInfoReceived = true;
                break;
            }
            if (request.body_case() ==
                panorama::wire::v1::Request::kPing) {
                auto response = baseResponse(request);
                response.mutable_pong()->set_payload(
                    request.ping().payload());
                if (!writeResponse(sockets[1], response, &peerError)) {
                    return;
                }
                continue;
            }
            if (request.body_case() ==
                panorama::wire::v1::Request::kUserConfigurationQuery) {
                auto response = baseResponse(request);
                auto *userConfig = response.mutable_user_configuration();
                userConfig->mutable_display_config()
                    ->set_backlight_brightness(75);
                userConfig->mutable_work_config()
                    ->set_single_mode_media_file("default_01.mp4.h264_2240x1080");
                userConfig->mutable_standby_config()->set_enable(true);
                if (!writeResponse(sockets[1], response, &peerError)) {
                    return;
                }
                continue;
            }
            if (request.body_case() !=
                    panorama::wire::v1::Request::kOverlayLayout &&
                request.body_case() !=
                    panorama::wire::v1::Request::kMetricBatch) {
                peerError = QStringLiteral(
                    "same-path test received request body %1 before device info")
                                .arg(static_cast<int>(request.body_case()));
                return;
            }
        }
        if (!deviceInfoReceived) {
            peerError = QStringLiteral(
                "same-path test did not receive device-info request");
            return;
        }
        requestSeen.store(true, std::memory_order_release);
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = QStringLiteral("old generation transport was not cancelled");
            return;
        }
        char byte = 0;
        if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) != 0) {
            peerError = QStringLiteral("old generation endpoint did not close cleanly");
        }
    });

    QElapsedTimer sessionTimer;
    sessionTimer.start();
    while (!manager->printerDisplaySessionActiveForTesting() &&
           sessionTimer.elapsed() < kPeerTimeoutMs * 3) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    const bool sessionWasActive =
        manager->printerDisplaySessionActiveForTesting();

    manager->requestDeviceInfo();
    QElapsedTimer readinessTimer;
    readinessTimer.start();
    while (!requestSeen.load(std::memory_order_acquire) &&
           readinessTimer.elapsed() < kPeerTimeoutMs * 3) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    const bool requestWasSeen =
        requestSeen.load(std::memory_order_acquire);
    resumeSpy.clear();

    manager->injectPrinterUdevEventForTesting(
        QByteArrayLiteral("usb"),
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/1-1")),
        QStringLiteral("1-1"));
    const quint64 newGeneration = manager->printerGenerationForTesting();
    const int operationsCancelledCount = operationsCancelledSpy.count();
    const int resumeCount = resumeSpy.count();

    QElapsedTimer cancellationTimer;
    cancellationTimer.start();
    while (rawFailureSpy.count() < 1 &&
           cancellationTimer.elapsed() < kPeerTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    const bool oldFailureObserved = rawFailureSpy.count() == 1;
    const int staleDeliveredCount = deliveredFailureSpy.count();

    manager->emitPrinterDeviceInfoFailureForTesting(
        QStringLiteral("current-generation-sentinel"), newGeneration);
    QElapsedTimer deliveryTimer;
    deliveryTimer.start();
    while (deliveredFailureSpy.count() < 1 &&
           deliveryTimer.elapsed() < kPeerTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(sessionWasActive, qPrintable(peerError));
    QVERIFY2(requestWasSeen, qPrintable(peerError));
    QCOMPARE(newGeneration, oldGeneration + 1);
    QCOMPARE(operationsCancelledCount, 1);
    QCOMPARE(resumeCount, 1);
    QCOMPARE(resumeSpy.first().at(0).toString(), endpointPath);
    QCOMPARE(resumeSpy.first().at(1).toULongLong(), newGeneration);
    QVERIFY(oldFailureObserved);
    QCOMPARE(rawFailureSpy.at(0).at(1).toULongLong(), oldGeneration);
    QVERIFY(rawFailureSpy.at(0).at(0).toString().contains(
        QStringLiteral("cancel"), Qt::CaseInsensitive));
    QCOMPARE(staleDeliveredCount, 0);
    QCOMPARE(deliveredFailureSpy.count(), 1);
    QCOMPARE(deliveredFailureSpy.at(0).at(0).toString(),
             QStringLiteral("current-generation-sentinel"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::printerSessionLossCancelsOperationsAndReportsStoppedState() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    const quint64 activeGeneration = manager->printerGenerationForTesting();

    QSignalSpy cancelledSpy(manager.get(),
                            &DeviceManager::printerOperationsCancelled);
    QSignalSpy disconnectedSpy(manager.get(), &DeviceManager::deviceDisconnected);
    QSignalSpy statusSpy(manager.get(), &DeviceManager::uploadStatus);
    statusSpy.clear();
    manager->emitPrinterSessionLostForTesting(activeGeneration);

    QTRY_COMPARE(cancelledSpy.count(), 1);
    QCOMPARE(disconnectedSpy.count(), 0);
    QVERIFY(manager->isPrinterClassConnected());
    QCOMPARE(manager->printerGenerationForTesting(), activeGeneration);
    QVERIFY(!manager->printerDisplaySessionActiveForTesting());
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(statusSpy.first().first().toString().contains(
        QStringLiteral("new USB endpoint generation"), Qt::CaseInsensitive));
}

void PrinterProtocolTests::lostPrinterSessionRejectsMutationsBeforeDispatch() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    const quint64 generation = manager->printerGenerationForTesting();
    manager->emitPrinterSessionLostForTesting(generation);
    QTRY_VERIFY(manager->printerDisplaySessionLost_);

    QTemporaryFile source;
    QVERIFY(source.open());
    QCOMPARE(source.write(QByteArrayLiteral("source")), 6);
    source.flush();
    QSignalSpy preparationSpy(manager.get(),
                              &DeviceManager::requestPreparePrinterMedia);

    const QString uploadId =
        QStringLiteral("61616161-6161-4161-8161-616161616161");
    QCOMPARE(manager->queueUploadOperation(uploadId, source.fileName()),
             uploadId);
    QCOMPARE(manager->operationInfo(uploadId).state,
             QStringLiteral("Failed"));
    QCOMPARE(manager->operationInfo(uploadId).errorCategory,
             QStringLiteral("SessionLost"));
    QCOMPARE(preparationSpy.count(), 0);

    TryxRuntimeApplyRequest applyRequest;
    applyRequest.media = {
        QStringLiteral("existing.mp4.h264_2240x1080")};
    applyRequest.ratio = QStringLiteral("2:1");
    applyRequest.screenMode = QStringLiteral("Full Screen");
    applyRequest.playMode = QStringLiteral("Single");
    const QString applyId =
        QStringLiteral("62626262-6262-4262-8262-626262626262");
    QCOMPARE(manager->queueApplyOperation(applyId, applyRequest), applyId);
    QCOMPARE(manager->operationInfo(applyId).errorCategory,
             QStringLiteral("SessionLost"));

    TryxRuntimeMetricsConfigRequest metricsRequest;
    metricsRequest.enabled = true;
    metricsRequest.metrics = {QStringLiteral("GPU Temperature")};
    const QString metricsId =
        QStringLiteral("63636363-6363-4363-8363-636363636363");
    QCOMPARE(manager->queueMetricsConfigOperation(metricsId, metricsRequest),
             metricsId);
    QCOMPARE(manager->operationInfo(metricsId).errorCategory,
             QStringLiteral("SessionLost"));
}

void PrinterProtocolTests::
    lostPrinterSessionRequiresObservedRemovalBeforeReconnect() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString usbName = QStringLiteral("1-1");
    const QString lpName = QStringLiteral("lp0");
    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    QSignalSpy startSpy(
        manager.get(), &DeviceManager::requestStartPrinterSession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    startSpy.clear();

    manager->emitPrinterSessionLostForTesting(
        manager->printerGenerationForTesting());
    QTRY_VERIFY(manager->printerDisplaySessionLost_);
    QVERIFY(!manager->printerSessionLossRemovalObserved_);

    manager->connectDevice();
    QCOMPARE(startSpy.count(), 0);
    QVERIFY(manager->printerDisplaySessionLost_);
    QVERIFY(!manager->printerSessionLossRemovalObserved_);

    const QString usbDevicePath =
        QDir(sysRoot).filePath(
            QStringLiteral("bus/usb/devices/") + usbName);
    const QString classPath =
        QDir(sysRoot).filePath(
            QStringLiteral("class/usbmisc/") + lpName);
    QVERIFY(QDir(usbDevicePath).removeRecursively());
    QVERIFY(QDir(classPath).removeRecursively());
    QVERIFY(QFile::remove(
        QDir(devRoot).filePath(
            QStringLiteral("usb/") + lpName)));
    manager->rescanPrinterForTesting();
    QVERIFY(manager->printerDisplaySessionLost_);
    QVERIFY(manager->printerSessionLossRemovalObserved_);
    QCOMPARE(startSpy.count(), 0);

    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(
        sysRoot, devRoot, usbName, lpName));
    manager->rescanPrinterForTesting();
    QCOMPARE(startSpy.count(), 1);
    QVERIFY(!manager->printerDisplaySessionLost_);
    QVERIFY(!manager->printerSessionLossRemovalObserved_);
}

void PrinterProtocolTests::
    sessionNotReadyRejectsMutationsBeforeDispatch() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestStartPrinterSession,
        manager->worker_, &DeviceWorker::startPrinterDisplaySession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    QVERIFY(!manager->printerDisplaySessionActive_);
    QVERIFY(!manager->printerDisplaySessionLost_);

    QSignalSpy preparationSpy(
        manager.get(), &DeviceManager::requestPreparePrinterMedia);
    QSignalSpy applySpy(
        manager.get(), &DeviceManager::requestPrinterApplyMedia);
    QSignalSpy metricsSpy(
        manager.get(), &DeviceManager::requestPrinterConfigureMetrics);
    QSignalSpy deleteSpy(
        manager.get(), &DeviceManager::requestPrinterDeleteMedia);
    QSignalSpy retryValidationSpy(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache);

    QTemporaryFile source;
    QVERIFY(source.open());
    QCOMPARE(source.write(QByteArrayLiteral("source")), 6);
    source.flush();

    const QString uploadId =
        QStringLiteral("69696969-6969-4969-8969-696969696961");
    QCOMPARE(manager->queueUploadOperation(
                 uploadId, source.fileName()),
             uploadId);
    QCOMPARE(manager->operationInfo(uploadId).errorCategory,
             QStringLiteral("SessionNotReady"));

    TryxRuntimeApplyRequest applyRequest;
    applyRequest.media = {
        QStringLiteral("existing.mp4.h264_2240x1080")};
    applyRequest.ratio = QStringLiteral("2:1");
    applyRequest.screenMode = QStringLiteral("Full Screen");
    applyRequest.playMode = QStringLiteral("Single");
    const QString applyId =
        QStringLiteral("69696969-6969-4969-8969-696969696962");
    QCOMPARE(manager->queueApplyOperation(applyId, applyRequest),
             applyId);
    QCOMPARE(manager->operationInfo(applyId).errorCategory,
             QStringLiteral("SessionNotReady"));

    TryxRuntimeMetricsConfigRequest metricsRequest;
    metricsRequest.enabled = true;
    metricsRequest.metrics = {
        QStringLiteral("GPU Temperature")};
    metricsRequest.alignment = QStringLiteral("Left");
    metricsRequest.textColor = 0xDCDCDCU;
    const QString metricsId =
        QStringLiteral("69696969-6969-4969-8969-696969696963");
    QCOMPARE(manager->queueMetricsConfigOperation(
                 metricsId, metricsRequest),
             metricsId);
    QCOMPARE(manager->operationInfo(metricsId).errorCategory,
             QStringLiteral("SessionNotReady"));

    const QString deleteId =
        QStringLiteral("69696969-6969-4969-8969-696969696964");
    QCOMPARE(manager->queueDeleteMediaOperation(
                 deleteId,
                 {QStringLiteral("existing.mp4.h264_2240x1080")}),
             deleteId);
    QCOMPARE(manager->operationInfo(deleteId).errorCategory,
             QStringLiteral("SessionNotReady"));

    const QString retrySourceId =
        QStringLiteral("69696969-6969-4969-8969-696969696965");
    DeviceManager::OperationRecord retrySource;
    retrySource.info.id = retrySourceId;
    retrySource.info.kind = QStringLiteral("Upload");
    retrySource.info.state = QStringLiteral("RetryAvailable");
    retrySource.info.retryMode = QStringLiteral("PreparedMedia");
    retrySource.info.subject = QStringLiteral("prepared.mp4");
    manager->operations_.insert(retrySourceId, retrySource);
    manager->operationOrder_.append(retrySourceId);

    const QString retryId =
        QStringLiteral("69696969-6969-4969-8969-696969696966");
    QCOMPARE(manager->retryOperation(retrySourceId, retryId),
             retryId);
    QCOMPARE(manager->operationInfo(retryId).errorCategory,
             QStringLiteral("SessionNotReady"));

    QCOMPARE(preparationSpy.count(), 0);
    QCOMPARE(applySpy.count(), 0);
    QCOMPARE(metricsSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);
    QCOMPARE(retryValidationSpy.count(), 0);
    QVERIFY(manager->activeOperationInfo().id.isEmpty());
}

void PrinterProtocolTests::
    persistentUsbInputFailureStopsSameGenerationWithoutRecovery() {
    constexpr quint64 generation = 70;
    const QString endpoint = QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"), generation);
    worker.printerSessionState_ =
        DeviceWorker::PrinterSessionState::Active;
    worker.printerOverlayActivationPending_ = true;
    worker.printerKeepaliveTimer_->start(10000);
    worker.printerMetricsTimer_->start(10000);
    worker.printerRecoveryTimer_->start(10000);
    worker.printerProtocol_
        ->setPersistentUsbInputFailureForTesting(true);

    QSignalSpy stoppedSpy(
        &worker, &DeviceWorker::printerSessionStopped);
    QSignalSpy lostSpy(
        &worker, &DeviceWorker::printerSessionLost);
    QSignalSpy errorSpy(
        &worker, &DeviceWorker::printerOperationError);
    QSignalSpy startedSpy(
        &worker, &DeviceWorker::printerSessionStarted);

    worker.schedulePrinterSessionRecovery(
        QStringLiteral("persistent input transport failure"),
        generation);

    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Lost);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(lostSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 0);
    QVERIFY(!worker.printerKeepaliveTimer_->isActive());
    QVERIFY(!worker.printerMetricsTimer_->isActive());
    QVERIFY(!worker.printerRecoveryTimer_->isActive());
    QVERIFY(!worker.printerOverlayActivationPending_);

    worker.startPrinterDisplaySession(endpoint, generation);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Lost);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(lostSpy.count(), 1);
    QVERIFY(!worker.printerRecoveryTimer_->isActive());

    const quint64 newGeneration = generation + 1;
    worker.updatePrinterGenerationGate(newGeneration, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"), newGeneration);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Passive);
    QVERIFY(!worker.printerProtocol_
                 ->persistentUsbInputFailure());
}

void PrinterProtocolTests::partialUploadRequiresObservedDeviceRemovalBeforeRetry() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString usbName = QStringLiteral("1-1");
    const QString lpName = QStringLiteral("lp0");
    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();

    const QString cacheDirectory = manager->retryCacheDirectory();
    QVERIFY(QDir().mkpath(cacheDirectory));
    const QString preparedPath =
        QDir(cacheDirectory).filePath(QStringLiteral("partial.h264"));
    const QByteArray preparedBytes(4096, '\x51');
    QFile preparedFile(preparedPath);
    QVERIFY(preparedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(preparedFile.write(preparedBytes),
             static_cast<qint64>(preparedBytes.size()));
    preparedFile.close();

    const QString operationId =
        QStringLiteral("64646464-6464-4464-8464-646464646464");
    DeviceManager::OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("Upload");
    record.info.state = QStringLiteral("Transferring");
    record.info.stage = QStringLiteral("Transferring");
    record.info.terminalOutcome =
        QStringLiteral("PartialOrUnknown");
    record.info.primaryErrorCategory =
        QStringLiteral("PartialOrUnknown");
    record.info.primaryErrorMessage =
        QStringLiteral("write outcome unknown");
    record.info.subject = QStringLiteral("partial.mp4");
    record.info.resultName =
        QStringLiteral("partial.mp4.h264_2240x1080");
    record.info.deviceGeneration = manager->printerGeneration_;
    record.preparedPath = preparedPath;
    record.preparedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(preparedBytes, QCryptographicHash::Sha256)
            .toHex());
    record.remoteName = record.info.resultName;
    record.originalRemoteName = record.remoteName;
    record.requiresDeviceRecovery = true;
    record.retryMustUseNewRemoteName = true;
    record.uploadDeviceIdentity =
        manager->printerDeviceSerial_.trimmed();
    record.uploadDeviceGeneration =
        manager->printerGeneration_;
    manager->operations_.insert(operationId, record);
    manager->operationOrder_.append(operationId);
    manager->activeOperationId_ = operationId;
    const quint64 generationBeforeRecovery = manager->printerGeneration_;
    manager->requirePrinterRecovery(QString());
    QCOMPARE(manager->printerGeneration_, generationBeforeRecovery + 1);
    QVERIFY(!manager->worker_->printerEndpointReady_.load(
        std::memory_order_acquire));
    manager->handlePreparedUploadFailure(
        operationId, QStringLiteral("write outcome unknown"),
        PrinterProtocol::MutationOutcome::NotStarted);

    QCOMPARE(manager->operationInfo(operationId).state,
             QStringLiteral("RetryAvailable"));
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manager->printerRecoveryRequired_);
    QFile manifestFile(manager->retryCacheManifestPath());
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifest =
        QJsonDocument::fromJson(manifestFile.readAll()).object();
    QCOMPARE(manifest.value(QStringLiteral("version")).toInt(), 9);
    QVERIFY(manifest.value(QStringLiteral("requiresDeviceRecovery")).toBool());
    QVERIFY(manifest.value(QStringLiteral("requiresNewRemoteName")).toBool());
    QCOMPARE(
        manifest.value(QStringLiteral("terminalOutcome")).toString(),
        QStringLiteral("PartialOrUnknown"));
    QCOMPARE(
        manifest.value(QStringLiteral("primaryErrorMessage")).toString(),
        QStringLiteral("write outcome unknown"));

    const QString blockedRetryId =
        QStringLiteral("65656565-6565-4565-8565-656565656565");
    QCOMPARE(manager->retryOperation(operationId, blockedRetryId),
             blockedRetryId);
    QCOMPARE(manager->operationInfo(blockedRetryId).errorCategory,
             QStringLiteral("DeviceRecoveryRequired"));

    const QString usbDevicePath =
        QDir(sysRoot).filePath(QStringLiteral("bus/usb/devices/") + usbName);
    const QString classPath =
        QDir(sysRoot).filePath(QStringLiteral("class/usbmisc/") + lpName);
    QVERIFY(QDir(usbDevicePath).removeRecursively());
    QVERIFY(QDir(classPath).removeRecursively());
    QVERIFY(QFile::remove(
        QDir(devRoot).filePath(QStringLiteral("usb/") + lpName)));
    manager->rescanPrinterForTesting();
    QVERIFY(manager->printerRecoveryRemovalObserved_);
    QVERIFY(manager->printerRecoveryRequired_);

    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));
    manager->rescanPrinterForTesting();
    QVERIFY(!manager->printerRecoveryRequired_);
    QVERIFY(!manager->printerRecoveryRemovalObserved_);
    manager->printerDisplaySessionActive_ = true;
    QCOMPARE(manager->operationInfo(operationId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QCOMPARE(manager->operationInfo(operationId).terminalOutcome,
             QStringLiteral("PartialOrUnknown"));
    QCOMPARE(manager->operationInfo(operationId).primaryErrorMessage,
             QStringLiteral("write outcome unknown"));

    const QString acceptedRetryId =
        QStringLiteral("66666666-6666-4666-8666-666666666667");
    QCOMPARE(manager->retryOperation(operationId, acceptedRetryId),
             acceptedRetryId);
    QCOMPARE(manager->operationInfo(acceptedRetryId).state,
             QStringLiteral("Preflight"));
}

void PrinterProtocolTests::tamperedV8RetryManifestIsRejected_data() {
    QTest::addColumn<QString>("terminalOutcome");
    QTest::addColumn<QString>("tamperKind");

    QTest::newRow("partial-without-fresh-name")
        << QStringLiteral("PartialOrUnknown")
        << QStringLiteral("clear-fresh-name");
    QTest::newRow("finalization-without-reconciliation")
        << QStringLiteral("FinalizationUnknown")
        << QStringLiteral("clear-finalization");
    QTest::newRow("not-started-with-confirmed-progress")
        << QStringLiteral("NotStarted")
        << QStringLiteral("inject-confirmed-progress");
}

void PrinterProtocolTests::tamperedV8RetryManifestIsRejected() {
    QFETCH(QString, terminalOutcome);
    QFETCH(QString, tamperKind);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString operationId =
        QStringLiteral("66666666-7777-4777-8777-888888888888");
    const QString remoteName =
        QStringLiteral("tampered.mp4.h264_2240x1080");
    const QByteArray preparedBytes(4096, '\x66');
    QString preparedPath;

    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        preparedPath = QDir(cacheDirectory).filePath(
            QStringLiteral("tampered.h264"));
        QVERIFY(writeTextFile(preparedPath, preparedBytes));

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.errorCategory = terminalOutcome;
        record.info.terminalOutcome = terminalOutcome;
        record.info.primaryErrorCategory = terminalOutcome;
        record.info.primaryErrorMessage =
            QStringLiteral("persisted transport outcome");
        record.info.subject = QStringLiteral("tampered.mp4");
        record.info.resultName = remoteName;
        record.info.total = preparedBytes.size();
        record.info.confirmedBytes =
            terminalOutcome == QStringLiteral("FinalizationUnknown")
                ? preparedBytes.size()
                : terminalOutcome ==
                          QStringLiteral("PartialOrUnknown")
                    ? 1024
                    : 0;
        record.info.lastConfirmedChunkIndex =
            record.info.confirmedBytes > 0 ? 0 : -1;
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = remoteName;
        record.originalRemoteName = remoteName;
        record.retryMustUseNewRemoteName =
            terminalOutcome == QStringLiteral("PartialOrUnknown");
        record.uploadFinalizationReconciliationPending =
            terminalOutcome == QStringLiteral("FinalizationUnknown");
        record.uploadDeviceIdentity = QStringLiteral("1-1");
        record.uploadDeviceGeneration = 7;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId, terminalOutcome, &cacheError),
                 qPrintable(cacheError));

        QFile manifestFile(writer->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        if (tamperKind == QStringLiteral("clear-fresh-name")) {
            manifest.insert(QStringLiteral("requiresNewRemoteName"),
                            false);
        } else if (tamperKind ==
                   QStringLiteral("clear-finalization")) {
            manifest.insert(
                QStringLiteral("finalizationOnlyReconciliation"),
                false);
            manifest.insert(QStringLiteral("requiresNewRemoteName"),
                            false);
        } else {
            manifest.insert(QStringLiteral("confirmedBytes"),
                            QStringLiteral("1024"));
            manifest.insert(QStringLiteral("lastConfirmedChunkIndex"),
                            0);
            manifest.insert(QStringLiteral("requiresNewRemoteName"),
                            false);
        }
        QSaveFile replacement(writer->retryCacheManifestPath());
        QVERIFY(replacement.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate));
        const QByteArray json =
            QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QCOMPARE(replacement.write(json),
                 static_cast<qint64>(json.size()));
        QVERIFY(replacement.commit());
    }

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    QSignalSpy validationSpy(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache);
    manager->loadRetryCache();

    QCOMPARE(validationSpy.count(), 0);
    QVERIFY(manager->pendingRetryValidationId_.isEmpty());
    QVERIFY(!QFileInfo::exists(manager->retryCacheManifestPath()));
    QVERIFY(!QFileInfo::exists(preparedPath));
}

void PrinterProtocolTests::legacyRetryManifestRestoresPartialTransferHazard() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString operationId =
        QStringLiteral("67676767-6767-4767-8767-676767676767");
    const QString usbName = QStringLiteral("1-1");
    const QString lpName = QStringLiteral("lp0");

    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        const QString preparedPath =
            QDir(cacheDirectory).filePath(QStringLiteral("legacy.h264"));
        const QByteArray preparedBytes(2048, '\x37');
        QFile preparedFile(preparedPath);
        QVERIFY(preparedFile.open(QIODevice::WriteOnly |
                                  QIODevice::Truncate));
        QCOMPARE(preparedFile.write(preparedBytes),
                 static_cast<qint64>(preparedBytes.size()));
        preparedFile.close();

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("UploadRetry");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.errorCategory = QStringLiteral("NotStarted");
        record.info.retryMode = QStringLiteral("PreparedMedia");
        record.info.subject = QStringLiteral("legacy.mp4");
        record.info.resultName =
            QStringLiteral("legacy.mp4.h264_2240x1080");
        record.info.attempt = 3;
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = record.info.resultName;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId, QStringLiteral("NotStarted"), &cacheError),
                 qPrintable(cacheError));

        QFile manifestFile(writer->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        manifest.insert(QStringLiteral("version"), 4);
        manifest.remove(QStringLiteral("requiresDeviceRecovery"));
        QSaveFile replacement(writer->retryCacheManifestPath());
        QVERIFY(replacement.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate));
        const QByteArray json =
            QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QCOMPARE(replacement.write(json),
                 static_cast<qint64>(json.size()));
        QVERIFY(replacement.commit());
    }

    QVERIFY(createUsbDevice(sysRoot, usbName, "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, usbName, lpName));

    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    manager->loadRetryCache();
    const QString validationId = manager->pendingRetryValidationId_;
    QVERIFY(!validationId.isEmpty());
    QSignalSpy sessionStartSpy(
        manager.get(), &DeviceManager::requestStartPrinterSession);
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    QVERIFY(manager->isPrinterClassConnected());
    QCOMPARE(sessionStartSpy.count(), 0);
    QVERIFY(!manager->worker_->printerEndpointReady_.load(
        std::memory_order_acquire));
    const quint64 generationBeforeRecovery = manager->printerGeneration_;
    manager->handleRetryCacheValidation(validationId, true, false,
                                        QString());
    QVERIFY(manager->printerRecoveryRequired_);
    QCOMPARE(manager->printerGeneration_, generationBeforeRecovery + 1);
    QCOMPARE(sessionStartSpy.count(), 0);
    QVERIFY(!manager->worker_->printerEndpointReady_.load(
        std::memory_order_acquire));
    QVERIFY(manager->operations_.value(manager->retryCacheOperationId_)
                .requiresDeviceRecovery);
    QCOMPARE(manager->operationInfo(manager->retryCacheOperationId_)
                 .errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manager->operationInfo(manager->retryCacheOperationId_)
                .message.contains(QStringLiteral("Power-cycle")));
}

void PrinterProtocolTests::legacyUnknownOutcomeRequiresObservedRecovery() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString operationId =
        QStringLiteral("67676767-6767-4767-8767-676767676768");
    const QString remoteName =
        QStringLiteral("unknown.mp4.h264_2240x1080");
    const QByteArray preparedBytes(4096, '\x67');

    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        const QString preparedPath =
            QDir(cacheDirectory).filePath(
                QStringLiteral("unknown-v6.h264"));
        QVERIFY(writeTextFile(preparedPath, preparedBytes));

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.errorCategory = QStringLiteral("NotStarted");
        record.info.terminalOutcome = QStringLiteral("NotStarted");
        record.info.primaryErrorCategory =
            QStringLiteral("NotStarted");
        record.info.primaryErrorMessage =
            QStringLiteral("legacy outcome");
        record.info.subject = QStringLiteral("unknown.mp4");
        record.info.resultName = remoteName;
        record.info.total = preparedBytes.size();
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = remoteName;
        record.originalRemoteName = remoteName;
        record.uploadDeviceIdentity = QStringLiteral("1-1");
        record.uploadDeviceGeneration = 9;
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId, QStringLiteral("NotStarted"),
                     &cacheError),
                 qPrintable(cacheError));

        QFile manifestFile(writer->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        manifest.insert(QStringLiteral("version"), 6);
        manifest.insert(QStringLiteral("terminalOutcome"),
                        QStringLiteral("UnknownLegacyOutcome"));
        manifest.insert(QStringLiteral("requiresDeviceRecovery"),
                        false);
        manifest.remove(QStringLiteral("originalRemoteName"));
        manifest.remove(QStringLiteral("retryRemoteName"));
        manifest.remove(QStringLiteral("primaryErrorCategory"));
        manifest.remove(QStringLiteral("primaryErrorMessage"));
        manifest.remove(QStringLiteral("confirmedBytes"));
        manifest.remove(QStringLiteral("lastConfirmedChunkIndex"));
        manifest.remove(QStringLiteral("requiresNewRemoteName"));
        manifest.remove(
            QStringLiteral("finalizationOnlyReconciliation"));
        manifest.remove(QStringLiteral("uploadDeviceGeneration"));
        QSaveFile replacement(writer->retryCacheManifestPath());
        QVERIFY(replacement.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate));
        const QByteArray json =
            QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QCOMPARE(replacement.write(json),
                 static_cast<qint64>(json.size()));
        QVERIFY(replacement.commit());
    }

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    manager->loadRetryCache();
    const QString validationId = manager->pendingRetryValidationId_;
    QVERIFY(!validationId.isEmpty());
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    manager->handleRetryCacheValidation(validationId, true, false,
                                        QString());

    const QString restoredId = manager->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    QCOMPARE(manager->operationInfo(restoredId).terminalOutcome,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manager->operations_.value(restoredId)
                .retryMustUseNewRemoteName);
    QVERIFY(manager->operations_.value(restoredId)
                .requiresDeviceRecovery);
    QVERIFY(manager->printerRecoveryRequired_);

    QFile migratedManifest(manager->retryCacheManifestPath());
    QVERIFY(migratedManifest.open(QIODevice::ReadOnly));
    const QJsonObject migrated =
        QJsonDocument::fromJson(migratedManifest.readAll()).object();
    QCOMPARE(migrated.value(QStringLiteral("version")).toInt(), 9);
    QCOMPARE(migrated.value(QStringLiteral("terminalOutcome")).toString(),
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(migrated.value(QStringLiteral("requiresNewRemoteName"))
                .toBool());
    QVERIFY(migrated.value(QStringLiteral("requiresDeviceRecovery"))
                .toBool());
}

void PrinterProtocolTests::recoveredV6ManifestMigratesToFreshNameRetry() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    const QString operationId =
        QStringLiteral("68686868-6868-4868-8868-686868686868");
    const QString retryId =
        QStringLiteral("68686868-6868-4868-8868-686868686869");
    const QString remoteName =
        QStringLiteral("recovered.mp4.h264_2240x1080");
    QString preparedPath;
    const QByteArray preparedBytes(4096, '\x68');

    {
        std::unique_ptr<DeviceManager> writer(
            DeviceManager::createForTesting(sysRoot, devRoot));
        const QString cacheDirectory = writer->retryCacheDirectory();
        QVERIFY(QDir().mkpath(cacheDirectory));
        preparedPath = QDir(cacheDirectory).filePath(
            QStringLiteral("recovered-v6.h264"));
        QVERIFY(writeTextFile(preparedPath, preparedBytes));

        DeviceManager::OperationRecord record;
        record.info.id = operationId;
        record.info.kind = QStringLiteral("Upload");
        record.info.state = QStringLiteral("RetryAvailable");
        record.info.stage = QStringLiteral("RetryAvailable");
        record.info.errorCategory =
            QStringLiteral("PartialOrUnknown");
        record.info.retryMode = QStringLiteral("PreparedMedia");
        record.info.subject = QStringLiteral("recovered.mp4");
        record.info.resultName = remoteName;
        record.info.total = preparedBytes.size();
        record.preparedPath = preparedPath;
        record.preparedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(preparedBytes,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.remoteName = remoteName;
        record.uploadDeviceIdentity = QStringLiteral("1-1");
        writer->operations_.insert(operationId, record);
        writer->operationOrder_.append(operationId);
        QString cacheError;
        QVERIFY2(writer->writeRetryCache(
                     operationId,
                     QStringLiteral("PartialOrUnknown"),
                     &cacheError),
                 qPrintable(cacheError));

        QFile manifestFile(writer->retryCacheManifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        QJsonObject manifest =
            QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        manifest.insert(QStringLiteral("version"), 6);
        manifest.insert(QStringLiteral("terminalOutcome"),
                        QStringLiteral("Recovered"));
        manifest.insert(QStringLiteral("requiresDeviceRecovery"),
                        false);
        manifest.remove(QStringLiteral("originalRemoteName"));
        manifest.remove(QStringLiteral("retryRemoteName"));
        manifest.remove(QStringLiteral("primaryErrorCategory"));
        manifest.remove(QStringLiteral("primaryErrorMessage"));
        manifest.remove(QStringLiteral("confirmedBytes"));
        manifest.remove(QStringLiteral("lastConfirmedChunkIndex"));
        manifest.remove(QStringLiteral("requiresNewRemoteName"));
        manifest.remove(
            QStringLiteral("finalizationOnlyReconciliation"));
        manifest.remove(QStringLiteral("uploadDeviceGeneration"));
        QSaveFile replacement(writer->retryCacheManifestPath());
        QVERIFY(replacement.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate));
        const QByteArray json =
            QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QCOMPARE(replacement.write(json),
                 static_cast<qint64>(json.size()));
        QVERIFY(replacement.commit());
    }

    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot,
                                  QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    std::unique_ptr<DeviceManager> manager(
        DeviceManager::createForTesting(sysRoot, devRoot));
    QObject::disconnect(
        manager.get(), &DeviceManager::requestValidatePrinterRetryCache,
        manager->printerMediaPreparer_,
        &PrinterMediaPreparer::validateRetryCache);
    manager->loadRetryCache();
    const QString validationId = manager->pendingRetryValidationId_;
    QVERIFY(!validationId.isEmpty());
    manager->setAutoConnectModeForTesting(true);
    manager->rescanPrinterForTesting();
    manager->handleRetryCacheValidation(validationId, true, false,
                                        QString());
    manager->printerDisplaySessionActive_ = true;

    const QString restoredId = manager->retryCacheOperationId_;
    QVERIFY(!restoredId.isEmpty());
    QVERIFY(!manager->printerRecoveryRequired_);
    QCOMPARE(manager->operationInfo(restoredId).terminalOutcome,
             QStringLiteral("PartialOrUnknown"));
    QCOMPARE(manager->operationInfo(restoredId).errorCategory,
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(manager->operations_.value(restoredId)
                .retryMustUseNewRemoteName);

    QFile migratedManifest(manager->retryCacheManifestPath());
    QVERIFY(migratedManifest.open(QIODevice::ReadOnly));
    const QJsonObject migrated =
        QJsonDocument::fromJson(migratedManifest.readAll()).object();
    QCOMPARE(migrated.value(QStringLiteral("version")).toInt(), 9);
    QCOMPARE(migrated.value(QStringLiteral("terminalOutcome")).toString(),
             QStringLiteral("PartialOrUnknown"));
    QVERIFY(migrated.value(QStringLiteral("requiresNewRemoteName"))
                .toBool());

    QSignalSpy uploadSpy(
        manager.get(), &DeviceManager::requestPrinterUploadPrepared);
    QCOMPARE(manager->retryOperation(restoredId, retryId), retryId);
    manager->printerMediaPreparer_->retryCacheValidated(
        retryId, true, false, QString());
    PrinterProtocol::MediaFile oldExact;
    oldExact.name = remoteName;
    oldExact.size = preparedBytes.size();
    oldExact.source = PrinterProtocol::MediaSource::User;
    manager->worker_->printerMediaListReady(
        retryId, {oldExact}, manager->printerGeneration_);
    QCOMPARE(uploadSpy.count(), 1);
    QVERIFY(uploadSpy.first().at(2).toString() != remoteName);
    QVERIFY(QFileInfo::exists(preparedPath));
}

void PrinterProtocolTests::monitoringFailureIsFailClosed() {
    PrinterDeviceMonitor monitor;
    QSignalSpy errorSpy(&monitor, &PrinterDeviceMonitor::monitorError);
    monitor.forceStartFailureForTesting();
    QVERIFY(!monitor.start());
    QCOMPARE(errorSpy.count(), 1);
    const PrinterProtocol::DiscoverySnapshot snapshot = monitor.snapshot();
    QVERIFY(snapshot.state == PrinterProtocol::DiscoveryState::MonitoringUnavailable);
    QVERIFY(snapshot.blocksLegacyTransport());
    QVERIFY(snapshot.statusText().contains(QStringLiteral("disabled"),
                                           Qt::CaseInsensitive));
}

void PrinterProtocolTests::typedPingTransaction() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() != panorama::wire::v1::Request::kPing ||
            request.ping().payload() != "hello?") {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("unexpected ping request");
            }
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("Hey!");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY2(protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("Hey!"));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::unframedTrackedResponseAfterTransportError() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError)) {
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("prefixTRYXsuffix");
        writeUnframedResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    protocol.setUnframedRecoveryEligibleForTesting(true);
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY2(protocol.trackedPingForTesting(
                 QStringLiteral("test-endpoint"), &payload, &error,
                 context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("prefixTRYXsuffix"));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::unframedTrackedResponseRequiresTransportError() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError)) {
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("raw-without-error");
        writeUnframedResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(150);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY(!protocol.trackedPingForTesting(
        QStringLiteral("test-endpoint"), &payload, &error, context));
    QVERIFY(!error.isEmpty());
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::unframedWrongTrackOrBodyDoesNotBypassMatching() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request firstRequest;
        if (!readRequest(sockets[1], &firstRequest, &peerError)) {
            return;
        }
        auto wrongTrack = baseResponse(firstRequest, 1);
        wrongTrack.mutable_pong()->set_payload("wrong-track");
        auto firstExpected = baseResponse(firstRequest);
        firstExpected.mutable_pong()->set_payload("first-ok");
        if (!writeUnframedResponse(sockets[1], wrongTrack, &peerError) ||
            !writeResponse(sockets[1], firstExpected, &peerError)) {
            return;
        }

        panorama::wire::v1::Request secondRequest;
        if (!readRequest(sockets[1], &secondRequest, &peerError)) {
            return;
        }
        auto wrongBody = baseResponse(secondRequest);
        wrongBody.mutable_acknowledgement();
        auto secondExpected = baseResponse(secondRequest);
        secondExpected.mutable_pong()->set_payload("second-ok");
        writeUnframedResponse(sockets[1], wrongBody, &peerError);
        writeResponse(sockets[1], secondExpected, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    protocol.setUnframedRecoveryEligibleForTesting(true);
    const PrinterProtocol::OperationContext context;
    QString payload;
    QString error;
    QVERIFY2(protocol.trackedPingForTesting(
                 QStringLiteral("test-endpoint"), &payload, &error,
                 context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("first-ok"));
    QVERIFY2(protocol.trackedPingForTesting(
                 QStringLiteral("test-endpoint"), &payload, &error,
                 context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("second-ok"));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapUsesExactSequence() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(sockets[1], &peerError,
                               &requestBuffer)) {
            return;
        }

        panorama::wire::v1::Request runConfigRequest;
        if (!readRequest(sockets[1], &runConfigRequest, &peerError,
                         kPeerTimeoutMs, &requestBuffer) ||
            runConfigRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout ||
            !runConfigRequest.has_header() ||
            runConfigRequest.header().track_id() == 0) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "bootstrap was not followed by tracked RunConfig");
            }
            return;
        }
        auto response = baseResponse(runConfigRequest);
        response.mutable_acknowledgement();
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::OperationContext context;
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), context);
    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.deviceInfo.productName, QStringLiteral("PANORAMA SE"));
    QCOMPARE(result.deviceInfo.firmwareVersion,
             QStringLiteral("test-firmware"));
    QCOMPARE(result.deviceInfo.serialNumber, QStringLiteral("test-serial"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapWaitsForLateDeviceInfo() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        const QByteArray expectedDeviceInfoFrame = QByteArray::fromHex(
            "545259580b0000000a020801a206040a024e41");
        panorama::wire::v1::Request deviceInfoRequest;
        QByteArray payload;
        if (!readFrameFd(sockets[1], &payload, &peerError) ||
            PrinterFrameCodec::encode(payload) != expectedDeviceInfoFrame ||
            !deviceInfoRequest.ParseFromArray(
                payload.constData(), static_cast<int>(payload.size()))) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "late readiness test did not receive exact DeviceInfo request");
            }
            return;
        }
        ++deviceInfoRequestCount;

        pollfd duplicateDescriptor{};
        duplicateDescriptor.fd = sockets[1];
        duplicateDescriptor.events = POLLIN;
        const int duplicatePoll = ::poll(&duplicateDescriptor, 1, 100);
        if (duplicatePoll < 0) {
            peerError = QStringLiteral(
                "late readiness duplicate check failed");
            return;
        }
        if (duplicatePoll > 0) {
            peerError = QStringLiteral(
                "DeviceInfo was repeated before the long readiness deadline");
            return;
        }

        auto deviceInfoResponse = baseResponse(deviceInfoRequest);
        auto *deviceInfo = deviceInfoResponse.mutable_device_information();
        deviceInfo->set_product_name("PANORAMA SE");
        deviceInfo->set_firmware_version("late-firmware");
        deviceInfo->set_serial_number("late-serial");
        if (!writeResponse(sockets[1], deviceInfoResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request sysConfigRequest;
        if (!readRequest(sockets[1], &sysConfigRequest, &peerError) ||
            sysConfigRequest.body_case() !=
                panorama::wire::v1::Request::kSystemConfigurationQuery) {
            peerError = QStringLiteral(
                "late readiness did not continue with SysConfig");
            return;
        }
        auto sysConfigResponse = baseResponse(sysConfigRequest);
        sysConfigResponse.mutable_system_configuration();
        if (!writeResponse(sockets[1], sysConfigResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request authRequest;
        if (!readRequest(sockets[1], &authRequest, &peerError) ||
            authRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceAuthenticationQuery) {
            peerError = QStringLiteral(
                "late readiness did not continue with DeviceAuth");
            return;
        }
        auto authResponse = baseResponse(authRequest);
        authResponse.mutable_device_authentication()->set_auth("ok");
        if (!writeResponse(sockets[1], authResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runConfigRequest;
        if (!readRequest(sockets[1], &runConfigRequest, &peerError) ||
            runConfigRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "late readiness did not continue with one RunConfig");
            return;
        }
        auto runConfigResponse = baseResponse(runConfigRequest);
        runConfigResponse.mutable_acknowledgement();
        writeResponse(sockets[1], runConfigResponse, &peerError);
    });

    PrinterProtocol protocol(40, 250);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(deviceInfoRequestCount, 1);
    QCOMPARE(result.deviceInfo.firmwareVersion,
             QStringLiteral("late-firmware"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapResynchronizesAfterStaleTail() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        // Reproduce a real restart: the previous process consumed a frame
        // header, while its protobuf tail remained queued in usblp. End with
        // a split magic prefix to verify that stream resynchronization keeps
        // a possible prefix across reads.
        const QByteArray staleTail = QByteArray::fromHex(
            "0a0c080110bdf6dfefe5a990a6291200c225040a026f6b");
        if (!writeAllFd(sockets[1],
                        staleTail +
                            QByteArrayLiteral("BYZLTRYX045168TR"),
                        &peerError)) {
            return;
        }

        panorama::wire::v1::Request deviceInfoRequest;
        if (!readRequest(sockets[1], &deviceInfoRequest, &peerError) ||
            deviceInfoRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "resynchronization test did not receive DeviceInfo");
            }
            return;
        }

        auto deviceInfoResponse = baseResponse(deviceInfoRequest);
        auto *deviceInfo = deviceInfoResponse.mutable_device_information();
        deviceInfo->set_product_name("PANORAMA SE");
        deviceInfo->set_firmware_version("resynchronized-firmware");
        deviceInfo->set_serial_number("resynchronized-serial");
        std::string serializedDeviceInfo;
        if (!deviceInfoResponse.SerializeToString(&serializedDeviceInfo)) {
            peerError = QStringLiteral(
                "resynchronization DeviceInfo serialization failed");
            return;
        }
        const QByteArray deviceInfoFrame = PrinterFrameCodec::encode(
            QByteArray(serializedDeviceInfo.data(),
                       static_cast<qsizetype>(serializedDeviceInfo.size())));
        if (!writeAllFd(sockets[1], deviceInfoFrame.mid(2), &peerError)) {
            return;
        }

        const auto respondToBootstrap = [&](auto expectedBody,
                                            auto populateResponse) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expectedBody) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "resynchronization bootstrap sequence was incomplete");
                }
                return false;
            }
            auto response = baseResponse(request);
            populateResponse(&response);
            return writeResponse(sockets[1], response, &peerError);
        };

        if (!respondToBootstrap(
                panorama::wire::v1::Request::kSystemConfigurationQuery,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_system_configuration();
                }) ||
            !respondToBootstrap(
                panorama::wire::v1::Request::kDeviceAuthenticationQuery,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_device_authentication()->set_auth("ok");
                }) ||
            !respondToBootstrap(
                panorama::wire::v1::Request::kOverlayLayout,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_acknowledgement();
                })) {
            return;
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.deviceInfo.firmwareVersion,
             QStringLiteral("resynchronized-firmware"));
    QCOMPARE(result.deviceInfo.serialNumber,
             QStringLiteral("resynchronized-serial"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapSkipsStaleTrackedResponse() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request deviceInfoRequest;
        if (!readRequest(sockets[1], &deviceInfoRequest, &peerError) ||
            deviceInfoRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "stale-response bootstrap did not receive DeviceInfo");
            }
            return;
        }

        // A tracked FileList response may arrive after its caller closed the
        // previous transport epoch. It is valid protobuf, but it does not
        // belong to the fixed track-zero bootstrap exchange.
        auto staleTracked = baseResponse(deviceInfoRequest, 77);
        staleTracked.mutable_media_catalog();
        if (!writeResponse(sockets[1], staleTracked, &peerError)) {
            return;
        }
        for (int index = 0; index < 40; ++index) {
            panorama::wire::v1::Response headerlessPong;
            headerlessPong.mutable_pong()->set_payload(
                QStringLiteral("late-pong-%1").arg(index).toStdString());
            if (!writeResponse(sockets[1], headerlessPong, &peerError)) {
                return;
            }
        }

        auto deviceInfoResponse = baseResponse(deviceInfoRequest);
        auto *deviceInfo = deviceInfoResponse.mutable_device_information();
        deviceInfo->set_product_name("PANORAMA SE");
        deviceInfo->set_firmware_version("stale-skipped-firmware");
        deviceInfo->set_serial_number("stale-skipped-serial");
        if (!writeResponse(sockets[1], deviceInfoResponse, &peerError)) {
            return;
        }

        const auto respond = [&](auto expectedBody, auto populateResponse) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expectedBody) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "stale-response bootstrap sequence was incomplete");
                }
                return false;
            }
            auto response = baseResponse(request);
            populateResponse(&response);
            return writeResponse(sockets[1], response, &peerError);
        };

        if (!respond(
                panorama::wire::v1::Request::kSystemConfigurationQuery,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_system_configuration();
                }) ||
            !respond(
                panorama::wire::v1::Request::kDeviceAuthenticationQuery,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_device_authentication()->set_auth("ok");
                }) ||
            !respond(
                panorama::wire::v1::Request::kOverlayLayout,
                [](panorama::wire::v1::Response *response) {
                    response->mutable_acknowledgement();
                })) {
            return;
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.deviceInfo.firmwareVersion,
             QStringLiteral("stale-skipped-firmware"));
    QCOMPARE(result.deviceInfo.serialNumber,
             QStringLiteral("stale-skipped-serial"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapRejectsUnboundedStaleResponses() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "bounded stale-response test did not receive DeviceInfo");
            }
            return;
        }
        ++deviceInfoRequestCount;
        for (int index = 0; index < 257; ++index) {
            panorama::wire::v1::Response pong;
            pong.mutable_pong()->set_payload("stale");
            if (!writeResponse(sockets[1], pong, &peerError)) {
                return;
            }
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY(!result.success);
    QCOMPARE(deviceInfoRequestCount, 1);
    QVERIFY(result.error.contains(QStringLiteral("Too many"),
                                  Qt::CaseInsensitive));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionDeviceInfoReadinessTimeoutSendsOnce() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "bounded readiness test did not receive DeviceInfo");
            }
            return;
        }
        ++deviceInfoRequestCount;

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, kPeerTimeoutMs) <= 0) {
            peerError = QStringLiteral(
                "bounded DeviceInfo readiness wait did not close its transport");
            return;
        }
        char byte = 0;
        const ssize_t peeked = ::recv(
            sockets[1], &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
        if (peeked > 0) {
            peerError = QStringLiteral(
                "bounded DeviceInfo readiness wait sent a duplicate request");
        } else if (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            peerError = QString::fromLocal8Bit(std::strerror(errno));
        }
    });

    PrinterProtocol protocol(40, 80);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY(!result.success);
    QCOMPARE(deviceInfoRequestCount, 1);
    QVERIFY(result.error.contains(QStringLiteral("1 attempts")));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbSessionBootstrapPartialResponseIsTerminal() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            peerError = QStringLiteral(
                "partial bootstrap test did not receive DeviceInfo");
            return;
        }
        ++deviceInfoRequestCount;

        auto response = baseResponse(request);
        response.mutable_device_information()->set_product_name("PANORAMA SE");
        std::string serialized;
        if (!response.SerializeToString(&serialized)) {
            peerError = QStringLiteral(
                "partial bootstrap response serialization failed");
            return;
        }
        const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
            serialized.data(), static_cast<qsizetype>(serialized.size())));
        const qsizetype partialSize = qMin<qsizetype>(10, frame.size() - 1);
        if (partialSize <= 8 ||
            ::send(sockets[1], frame.constData(),
                   static_cast<size_t>(partialSize), MSG_NOSIGNAL) !=
                partialSize) {
            peerError = QStringLiteral(
                "failed to send partial bootstrap response");
            return;
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, kPeerTimeoutMs) <= 0) {
            peerError = QStringLiteral(
                "partial bootstrap response did not close its transport");
            return;
        }
        char byte = 0;
        const ssize_t received = ::recv(
            sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
        if (received > 0) {
            ++deviceInfoRequestCount;
            peerError = QStringLiteral(
                "partial bootstrap response incorrectly triggered a retry");
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            peerError = QString::fromLocal8Bit(std::strerror(errno));
        }
    });

    PrinterProtocol protocol(60);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY(!result.success);
    QCOMPARE(deviceInfoRequestCount, 1);
    QVERIFY(result.error.contains(QStringLiteral("incomplete"),
                                  Qt::CaseInsensitive));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
udbSessionDeviceInfoRetriesConfirmedZeroByteOutWithBackoff() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    int sysConfigRequestCount = 0;
    int authRequestCount = 0;
    int runConfigRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request deviceInfoRequest;
        if (!readRequest(
                sockets[1], &deviceInfoRequest,
                &peerError, 3000) ||
            deviceInfoRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            peerError = QStringLiteral(
                "zero-byte readiness retry did not send DeviceInfo");
            return;
        }
        ++deviceInfoRequestCount;
        auto deviceInfoResponse =
            baseResponse(deviceInfoRequest);
        auto *deviceInfo =
            deviceInfoResponse.mutable_device_information();
        deviceInfo->set_product_name("PANORAMA SE");
        deviceInfo->set_firmware_version(
            "zero-byte-retry-firmware");
        if (!writeResponse(
                sockets[1], deviceInfoResponse,
                &peerError)) {
            return;
        }

        panorama::wire::v1::Request sysConfigRequest;
        if (!readRequest(
                sockets[1], &sysConfigRequest,
                &peerError) ||
            sysConfigRequest.body_case() !=
                panorama::wire::v1::Request::kSystemConfigurationQuery) {
            peerError = QStringLiteral(
                "zero-byte readiness retry did not send SysConfig");
            return;
        }
        ++sysConfigRequestCount;
        auto sysConfigResponse =
            baseResponse(sysConfigRequest);
        sysConfigResponse.mutable_system_configuration();
        if (!writeResponse(
                sockets[1], sysConfigResponse,
                &peerError)) {
            return;
        }

        panorama::wire::v1::Request authRequest;
        if (!readRequest(
                sockets[1], &authRequest,
                &peerError) ||
            authRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceAuthenticationQuery) {
            peerError = QStringLiteral(
                "zero-byte readiness retry did not send DeviceAuth");
            return;
        }
        ++authRequestCount;
        auto authResponse = baseResponse(authRequest);
        authResponse.mutable_device_authentication()->set_auth("ok");
        if (!writeResponse(
                sockets[1], authResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runConfigRequest;
        if (!readRequest(
                sockets[1], &runConfigRequest,
                &peerError) ||
            runConfigRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "zero-byte readiness retry did not send RunConfig");
            return;
        }
        ++runConfigRequestCount;
        auto runConfigResponse =
            baseResponse(runConfigRequest);
        runConfigResponse.mutable_acknowledgement();
        writeResponse(
            sockets[1], runConfigResponse, &peerError);
    });

    PrinterProtocol protocol(100, 1900);
    protocol.setBootstrapZeroByteWriteFailuresForTesting(2);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    QList<PrinterProtocol::ReadinessRetryInfo> retries;
    PrinterProtocol::OperationContext context;
    context.onReadinessProbeRetry =
        [&retries](
            const PrinterProtocol::ReadinessRetryInfo &retry) {
            retries.append(retry);
        };
    const PrinterProtocol::Result result =
        protocol.startDisplaySession(
            QStringLiteral("test-endpoint"), context);
    const QList<qint64> attemptOffsets =
        protocol.bootstrapReadinessAttemptOffsetsForTesting();

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(deviceInfoRequestCount, 1);
    QCOMPARE(sysConfigRequestCount, 1);
    QCOMPARE(authRequestCount, 1);
    QCOMPARE(runConfigRequestCount, 1);
    QCOMPARE(attemptOffsets.size(), 3);
    QVERIFY(attemptOffsets.at(0) < 100);
    QVERIFY(attemptOffsets.at(1) - attemptOffsets.at(0) >= 450);
    QVERIFY(attemptOffsets.at(1) - attemptOffsets.at(0) < 800);
    QVERIFY(attemptOffsets.at(2) - attemptOffsets.at(1) >= 950);
    QVERIFY(attemptOffsets.at(2) - attemptOffsets.at(1) < 1300);
    QCOMPARE(retries.size(), 2);
    QCOMPARE(retries.at(0).attempt, 1);
    QCOMPARE(retries.at(0).expectedBytes,
             static_cast<qint64>(19));
    QCOMPARE(retries.at(0).actualBytes,
             static_cast<qint64>(0));
    QCOMPARE(retries.at(0).backoffMs, 500);
    QVERIFY(retries.at(0).transferStatus.contains(
        QStringLiteral("zero-byte"),
        Qt::CaseInsensitive));
    QCOMPARE(retries.at(1).attempt, 2);
    QCOMPARE(retries.at(1).expectedBytes,
             static_cast<qint64>(19));
    QCOMPARE(retries.at(1).actualBytes,
             static_cast<qint64>(0));
    QCOMPARE(retries.at(1).backoffMs, 1000);
    QVERIFY(retries.at(1).elapsedMs >=
            retries.at(0).elapsedMs);
    QCOMPARE(
        result.deviceInfo.firmwareVersion,
        QStringLiteral("zero-byte-retry-firmware"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
udbSessionDeviceInfoCancellationDuringBackoffIsTerminal() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));
    int cancellationPipe[2] = {-1, -1};
    QVERIFY(::pipe2(
                cancellationPipe,
                O_CLOEXEC | O_NONBLOCK) == 0);

    std::thread cancel([&]() {
        ::poll(nullptr, 0, 100);
        const char signal = 1;
        ::write(cancellationPipe[1], &signal, 1);
    });

    PrinterProtocol protocol(100, 2000);
    protocol.setBootstrapZeroByteWriteFailuresForTesting(1);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationPipe[0];
    QElapsedTimer elapsed;
    elapsed.start();
    const PrinterProtocol::Result result =
        protocol.startDisplaySession(
            QStringLiteral("test-endpoint"), context);
    const qint64 elapsedMs = elapsed.elapsed();
    const QList<qint64> attemptOffsets =
        protocol.bootstrapReadinessAttemptOffsetsForTesting();

    cancel.join();
    ::close(cancellationPipe[0]);
    ::close(cancellationPipe[1]);

    char unexpected = 0;
    const ssize_t received = ::recv(
        sockets[1], &unexpected, sizeof(unexpected),
        MSG_DONTWAIT);
    ::close(sockets[1]);

    QVERIFY(!result.success);
    QVERIFY(result.error.contains(
        QStringLiteral("cancelled"),
        Qt::CaseInsensitive));
    QCOMPARE(attemptOffsets.size(), 1);
    QVERIFY(elapsedMs >= 50);
    QVERIFY(elapsedMs < 450);
    QCOMPARE(received, static_cast<ssize_t>(0));
}

void PrinterProtocolTests::
udbSessionBootstrapSendsPostReadinessExchangesOnce() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int deviceInfoRequestCount = 0;
    int sysConfigRequestCount = 0;
    int authRequestCount = 0;
    int runConfigRequestCount = 0;
    std::atomic<int> deviceInfoReadyCallbackCount{0};
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request deviceInfoRequest;
        if (!readRequest(sockets[1], &deviceInfoRequest, &peerError) ||
            deviceInfoRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceInformationQuery) {
            peerError = QStringLiteral(
                "single bootstrap did not receive DeviceInfo");
            return;
        }
        ++deviceInfoRequestCount;
        auto deviceInfoResponse = baseResponse(deviceInfoRequest);
        auto *deviceInfo = deviceInfoResponse.mutable_device_information();
        deviceInfo->set_product_name("PANORAMA SE");
        deviceInfo->set_firmware_version("exchange-firmware");
        if (!writeResponse(sockets[1], deviceInfoResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request sysConfigRequest;
        if (!readRequest(sockets[1], &sysConfigRequest, &peerError) ||
            sysConfigRequest.body_case() !=
                panorama::wire::v1::Request::kSystemConfigurationQuery) {
            peerError = QStringLiteral(
                "single bootstrap did not receive SysConfig");
            return;
        }
        if (deviceInfoReadyCallbackCount.load() != 1) {
            peerError = QStringLiteral(
                "DeviceInfo readiness callback did not precede SysConfig");
            return;
        }
        ++sysConfigRequestCount;
        auto sysConfigResponse = baseResponse(sysConfigRequest);
        sysConfigResponse.mutable_system_configuration();
        if (!writeResponse(
                sockets[1], sysConfigResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request authRequest;
        if (!readRequest(sockets[1], &authRequest, &peerError) ||
            authRequest.body_case() !=
                panorama::wire::v1::Request::kDeviceAuthenticationQuery) {
            peerError = QStringLiteral(
                "single bootstrap did not receive DeviceAuth");
            return;
        }
        ++authRequestCount;
        auto authResponse = baseResponse(authRequest);
        authResponse.mutable_device_authentication()->set_auth("ok");
        if (!writeResponse(sockets[1], authResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runConfigRequest;
        if (!readRequest(sockets[1], &runConfigRequest, &peerError) ||
            runConfigRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "single bootstrap did not continue with RunConfig");
            return;
        }
        ++runConfigRequestCount;
        auto runConfigResponse = baseResponse(runConfigRequest);
        runConfigResponse.mutable_acknowledgement();
        writeResponse(sockets[1], runConfigResponse, &peerError);
    });

    PrinterProtocol protocol(100);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.onDeviceInfoReady = [&]() {
        deviceInfoReadyCallbackCount.fetch_add(1);
    };
    const PrinterProtocol::Result result = protocol.startDisplaySession(
        QStringLiteral("test-endpoint"), context);

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(deviceInfoRequestCount, 1);
    QCOMPARE(sysConfigRequestCount, 1);
    QCOMPARE(authRequestCount, 1);
    QCOMPARE(runConfigRequestCount, 1);
    QCOMPARE(deviceInfoReadyCallbackCount.load(), 1);
    QCOMPARE(result.deviceInfo.firmwareVersion,
             QStringLiteral("exchange-firmware"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbKeepaliveUsesExactUntrackedFrame() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        QByteArray payload;
        if (!readFrameFd(sockets[1], &payload, &peerError)) {
            return;
        }
        const QByteArray expected = QByteArray::fromHex(
            "0a0052080a0668656c6c6f3f");
        if (payload != expected) {
            peerError = QStringLiteral("keepalive does not match UDB 2.3.1: %1")
                            .arg(QString::fromLatin1(payload.toHex()));
            return;
        }

        panorama::wire::v1::Request request;
        if (!request.ParseFromArray(payload.constData(),
                                    static_cast<int>(payload.size())) ||
            !request.has_header() || request.header().ByteSizeLong() != 0 ||
            request.body_case() != panorama::wire::v1::Request::kPing ||
            request.ping().payload() != "hello?") {
            peerError = QStringLiteral("invalid untracked keepalive protobuf");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendKeepalive(QStringLiteral("test-endpoint"), &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbKeepaliveDrainsOptionalPong() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int responseReadyFd = eventfd(0, EFD_CLOEXEC);
    QVERIFY(responseReadyFd >= 0);

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request firstRequest;
        if (!readRequest(sockets[1], &firstRequest, &peerError) ||
            !firstRequest.has_header() ||
            firstRequest.header().ByteSizeLong() != 0 ||
            firstRequest.body_case() != panorama::wire::v1::Request::kPing) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing first UDB keepalive");
            }
            return;
        }

        panorama::wire::v1::Response response;
        response.mutable_header();
        response.mutable_pong()->set_payload("Hey!");
        if (!writeResponse(sockets[1], response, &peerError)) {
            return;
        }
        panorama::wire::v1::Response staleResponse;
        staleResponse.mutable_header()->set_version(1);
        staleResponse.mutable_header()->set_track_id(999);
        staleResponse.mutable_acknowledgement()->set_dummy("stale");
        if (!writeResponse(sockets[1], staleResponse, &peerError)) {
            return;
        }
        const uint64_t readyValue = 1;
        if (::write(responseReadyFd, &readyValue, sizeof(readyValue)) !=
            static_cast<ssize_t>(sizeof(readyValue))) {
            peerError = QStringLiteral("optional Pong readiness signal failed");
            return;
        }

        panorama::wire::v1::Request secondRequest;
        if (!readRequest(sockets[1], &secondRequest, &peerError) ||
            !secondRequest.has_header() ||
            secondRequest.header().ByteSizeLong() != 0 ||
            secondRequest.body_case() != panorama::wire::v1::Request::kPing) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing second UDB keepalive");
            }
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendKeepalive(QStringLiteral("test-endpoint"), &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);

    pollfd readyDescriptor{};
    readyDescriptor.fd = responseReadyFd;
    readyDescriptor.events = POLLIN;
    QCOMPARE(::poll(&readyDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t readyValue = 0;
    QCOMPARE(::read(responseReadyFd, &readyValue, sizeof(readyValue)),
             static_cast<ssize_t>(sizeof(readyValue)));
    QCOMPARE(readyValue, uint64_t(1));

    QCOMPARE(protocol.sendKeepalive(QStringLiteral("test-endpoint"), &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    pollfd endpointDescriptor{};
    endpointDescriptor.fd = sockets[0];
    endpointDescriptor.events = POLLIN;
    QCOMPARE(::poll(&endpointDescriptor, 1, 0), 0);

    peer.join();
    ::close(responseReadyFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::udbKeepaliveZeroByteWriteFailureIsRetryable() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int sendBufferSize = 1024;
    QVERIFY(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                         &sendBufferSize, sizeof(sendBufferSize)) == 0);
    const int flags = ::fcntl(sockets[0], F_GETFL, 0);
    QVERIFY(flags >= 0);
    QVERIFY(::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);

    const QByteArray filler(4096, 'x');
    bool saturated = false;
    for (int attempt = 0; attempt < 4096; ++attempt) {
        const ssize_t written =
            ::write(sockets[0], filler.constData(),
                    static_cast<size_t>(filler.size()));
        if (written > 0) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            saturated = true;
            break;
        }
        QFAIL("failed to saturate keepalive test socket");
    }
    QVERIFY(saturated);

    PrinterProtocol protocol(20);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendKeepalive(QStringLiteral("test-endpoint"), &error, context),
             PrinterProtocol::KeepaliveOutcome::RetryableFailure);
    QVERIFY(error.contains(QStringLiteral("writing"), Qt::CaseInsensitive));

    ::close(sockets[1]);
}

void PrinterProtocolTests::displayKeepaliveIsWriteDrivenAndDrainsOptionalAck() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    const int responseReadyFd = ::eventfd(0, EFD_CLOEXEC);
    QVERIFY(responseReadyFd >= 0);

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request firstRequest;
        if (!readRequest(sockets[1], &firstRequest, &peerError) ||
            !firstRequest.has_header() ||
            firstRequest.header().version() != 0 ||
            firstRequest.header().track_id() != 0 ||
            firstRequest.header().payload_crc32() != 0 ||
            firstRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "first display keepalive did not match UDB RunConfig");
            }
            return;
        }
        auto firstResponse = baseResponse(firstRequest);
        firstResponse.mutable_acknowledgement();
        std::string serializedResponse;
        if (!firstResponse.SerializeToString(&serializedResponse)) {
            peerError = QStringLiteral(
                "failed to serialize optional RunConfig response");
            return;
        }
        const QByteArray responseFrame = PrinterFrameCodec::encode(
            QByteArray(serializedResponse.data(),
                       static_cast<qsizetype>(serializedResponse.size())));
        if (!writeAllFd(sockets[1], responseFrame.first(8), &peerError) ||
            !writeAllFd(sockets[1], responseFrame.sliced(8), &peerError)) {
            return;
        }
        const uint64_t readyValue = 1;
        if (::write(responseReadyFd, &readyValue, sizeof(readyValue)) !=
            static_cast<ssize_t>(sizeof(readyValue))) {
            peerError = QStringLiteral(
                "failed to signal optional RunConfig response");
            return;
        }

        panorama::wire::v1::Request secondRequest;
        if (!readRequest(sockets[1], &secondRequest, &peerError) ||
            !secondRequest.has_header() ||
            secondRequest.header().version() != 0 ||
            secondRequest.header().track_id() != 0 ||
            secondRequest.header().payload_crc32() != 0 ||
            secondRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "second display keepalive did not preserve the connection");
            }
            return;
        }
    });

    PrinterProtocol protocol(50);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    pollfd readyDescriptor{};
    readyDescriptor.fd = responseReadyFd;
    readyDescriptor.events = POLLIN;
    QCOMPARE(::poll(&readyDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t readyValue = 0;
    QCOMPARE(::read(responseReadyFd, &readyValue, sizeof(readyValue)),
             static_cast<ssize_t>(sizeof(readyValue)));
    QCOMPARE(readyValue, uint64_t(1));
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    peer.join();
    ::close(responseReadyFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::displayKeepaliveTransportFailureIsRetryable() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int sendBufferSize = 1024;
    QVERIFY(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                         &sendBufferSize, sizeof(sendBufferSize)) == 0);
    const int flags = ::fcntl(sockets[0], F_GETFL, 0);
    QVERIFY(flags >= 0);
    QVERIFY(::fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);

    const QByteArray filler(4096, 'x');
    bool saturated = false;
    for (int attempt = 0; attempt < 4096; ++attempt) {
        const ssize_t written =
            ::write(sockets[0], filler.constData(),
                    static_cast<size_t>(filler.size()));
        if (written > 0) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            saturated = true;
            break;
        }
        QFAIL("failed to saturate display keepalive test socket");
    }
    QVERIFY(saturated);

    PrinterProtocol protocol(20);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::RetryableFailure);
    QVERIFY(error.contains(QStringLiteral("writing"),
                           Qt::CaseInsensitive));

    ::close(sockets[1]);
}

void PrinterProtocolTests::displayKeepaliveMalformedResponseIsDiscarded() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    const int responseReadyFd = ::eventfd(0, EFD_CLOEXEC);
    QVERIFY(responseReadyFd >= 0);
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "display keepalive did not send RunConfig");
            }
            return;
        }
        const QByteArray malformedPayload(1, char(0x0f));
        const QByteArray frame = PrinterFrameCodec::encode(malformedPayload);
        if (!writeAllFd(sockets[1], frame, &peerError)) {
            return;
        }
        const uint64_t readyValue = 1;
        if (::write(responseReadyFd, &readyValue, sizeof(readyValue)) !=
            static_cast<ssize_t>(sizeof(readyValue))) {
            peerError = QStringLiteral(
                "failed to signal malformed RunConfig response");
            return;
        }

        panorama::wire::v1::Request secondRequest;
        if (!readRequest(sockets[1], &secondRequest, &peerError) ||
            secondRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "second display keepalive did not preserve the connection");
            }
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    pollfd readyDescriptor{};
    readyDescriptor.fd = responseReadyFd;
    readyDescriptor.events = POLLIN;
    QCOMPARE(::poll(&readyDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t readyValue = 0;
    QCOMPARE(::read(responseReadyFd, &readyValue, sizeof(readyValue)),
             static_cast<ssize_t>(sizeof(readyValue)));
    QCOMPARE(readyValue, uint64_t(1));
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    peer.join();
    ::close(responseReadyFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::displayKeepaliveIncompleteResponseIsDiscarded() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    const int responseReadyFd = ::eventfd(0, EFD_CLOEXEC);
    QVERIFY(responseReadyFd >= 0);
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request firstRequest;
        if (!readRequest(sockets[1], &firstRequest, &peerError) ||
            firstRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "first display keepalive did not send RunConfig");
            }
            return;
        }

        const QByteArray incompleteFrame =
            PrinterFrameCodec::encode(QByteArray(32, char(0x5a))).first(8);
        if (!writeAllFd(sockets[1], incompleteFrame, &peerError)) {
            return;
        }
        const uint64_t readyValue = 1;
        if (::write(responseReadyFd, &readyValue, sizeof(readyValue)) !=
            static_cast<ssize_t>(sizeof(readyValue))) {
            peerError = QStringLiteral(
                "failed to signal incomplete RunConfig response");
            return;
        }

        panorama::wire::v1::Request secondRequest;
        if (!readRequest(sockets[1], &secondRequest, &peerError) ||
            secondRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "second display keepalive did not preserve the connection");
            }
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    pollfd readyDescriptor{};
    readyDescriptor.fd = responseReadyFd;
    readyDescriptor.events = POLLIN;
    QCOMPARE(::poll(&readyDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t readyValue = 0;
    QCOMPARE(::read(responseReadyFd, &readyValue, sizeof(readyValue)),
             static_cast<ssize_t>(sizeof(readyValue)));
    QCOMPARE(readyValue, uint64_t(1));
    QCOMPARE(protocol.sendDisplayKeepalive(QStringLiteral("test-endpoint"),
                                           &error, context),
             PrinterProtocol::KeepaliveOutcome::Sent);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    peer.join();
    ::close(responseReadyFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::notificationBeforeExpectedResponse() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError)) {
            return;
        }
        auto notification = baseResponse(request);
        notification.mutable_asynchronous_event()->set_play_finished(true);
        if (!writeResponse(sockets[1], notification, &peerError)) {
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("after-notification");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY2(protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("after-notification"));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::headerlessPongBeforeTrackedResponseIsSkipped() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing tracked file-list request");
            }
            return;
        }

        panorama::wire::v1::Response latePong;
        latePong.mutable_pong()->set_payload("late");
        if (!writeResponse(sockets[1], latePong, &peerError)) {
            return;
        }

        auto response = baseResponse(request);
        auto *file = response.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/after-pong.png");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(321);
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const PrinterProtocol::OperationContext context;
    const auto result =
        protocol.readMediaList(QStringLiteral("test-endpoint"), context);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 1);
    QCOMPARE(result.files.first().name,
             QStringLiteral("after-pong.png.h264_2240x1080"));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::boundedResponseBurstBeforeFileListIsSkipped() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    int fileListRequestCount = 0;
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "bounded burst test did not receive FileList");
            }
            return;
        }
        ++fileListRequestCount;
        for (int index = 0; index < 40; ++index) {
            panorama::wire::v1::Response pong;
            pong.mutable_pong()->set_payload("queued-pong");
            if (!writeResponse(sockets[1], pong, &peerError)) {
                return;
            }
        }
        auto response = baseResponse(request);
        auto *file = response.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/recovered.mp4");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(4096);
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    const PrinterProtocol::MediaListResult result = protocol.readMediaList(
        QStringLiteral("test-endpoint"), {});

    peer.join();
    ::close(sockets[1]);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(fileListRequestCount, 1);
    QCOMPARE(result.files.size(), 1);
    QCOMPARE(result.files.first().name,
             QStringLiteral("recovered.mp4.h264_2240x1080"));
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::slowTrackedResponseGetsInFlightKeepalive() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    qint64 keepaliveDelayMs = -1;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing slow file-list request");
            }
            return;
        }

        QElapsedTimer idleTimer;
        idleTimer.start();
        panorama::wire::v1::Request keepalive;
        if (!readRequest(sockets[1], &keepalive, &peerError, 3000) ||
            !keepalive.has_header() ||
            keepalive.header().ByteSizeLong() != 0 ||
            keepalive.body_case() != panorama::wire::v1::Request::kPing ||
            keepalive.ping().payload() != "hello?") {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing in-flight UDB keepalive");
            }
            return;
        }
        keepaliveDelayMs = idleTimer.elapsed();

        auto response = baseResponse(request);
        auto *file = response.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/slow.png");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(654);
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(4000);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.maintainKeepalive = true;
    const auto result =
        protocol.readMediaList(QStringLiteral("test-endpoint"), context);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 1);

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY(keepaliveDelayMs >= 1500);
    QVERIFY(keepaliveDelayMs < 3000);
}

void PrinterProtocolTests::unrelatedTrackIsSkipped() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError)) {
            return;
        }
        auto unrelated = baseResponse(request, 1);
        unrelated.mutable_pong()->set_payload("wrong-track");
        if (!writeResponse(sockets[1], unrelated, &peerError)) {
            return;
        }
        auto response = baseResponse(request);
        response.mutable_pong()->set_payload("matching-track");
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY2(protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context),
             qPrintable(error));
    QCOMPARE(payload, QStringLiteral("matching-track"));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::timeoutIsBounded() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    PrinterProtocol protocol(40);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QElapsedTimer timer;
    timer.start();
    QString payload;
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY(!protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context));
    QVERIFY(error.contains(QStringLiteral("Timed out"), Qt::CaseInsensitive));
    QVERIFY(timer.elapsed() < 500);
    ::close(sockets[1]);
}

void PrinterProtocolTests::cancellationFdStopsTransaction() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int cancellationFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    QVERIFY(cancellationFd >= 0);
    const uint64_t value = 1;
    QCOMPARE(::write(cancellationFd, &value, sizeof(value)),
             static_cast<ssize_t>(sizeof(value)));

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationFd;
    QString payload;
    QString error;
    QVERIFY(!protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context));
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    char byte = 0;
    errno = 0;
    QCOMPARE(::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT),
             static_cast<ssize_t>(-1));
    QVERIFY(errno == EAGAIN || errno == EWOULDBLOCK);

    ::close(cancellationFd);
    ::close(sockets[1]);
}

void PrinterProtocolTests::cancellationFdInterruptsBlockedResponse() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int cancellationFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    QVERIFY(cancellationFd >= 0);

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() != panorama::wire::v1::Request::kPing) {
            peerError = QStringLiteral("peer did not receive ping before cancellation");
            return;
        }
        const uint64_t value = 1;
        if (::write(cancellationFd, &value, sizeof(value)) !=
            static_cast<ssize_t>(sizeof(value))) {
            peerError = QStringLiteral("failed to signal cancellation eventfd");
            return;
        }
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = QStringLiteral("transport did not close after blocked cancellation");
            return;
        }
        char byte = 0;
        const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
        if (received > 0) {
            peerError = QStringLiteral("unexpected bytes after blocked cancellation");
        }
    });

    PrinterProtocol protocol(2000);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationFd;
    QElapsedTimer timer;
    timer.start();
    QString payload;
    QString error;
    QVERIFY(!protocol.trackedPingForTesting(QStringLiteral("test-endpoint"), &payload,
                                            &error, context));
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    QVERIFY(timer.elapsed() < 500);
    peer.join();
    ::close(cancellationFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::userCancellationDoesNotInterruptSessionRecovery() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int requestSeenFd = eventfd(0, EFD_CLOEXEC);
    const int cancellationIssuedFd = eventfd(0, EFD_CLOEXEC);
    QVERIFY(requestSeenFd >= 0);
    QVERIFY(cancellationIssuedFd >= 0);

    QString peerError;
    std::thread peer([&]() {
        const QList<panorama::wire::v1::Request::BodyCase> expectedBodies = {
            panorama::wire::v1::Request::kDeviceInformationQuery,
            panorama::wire::v1::Request::kSystemConfigurationQuery,
            panorama::wire::v1::Request::kDeviceAuthenticationQuery
        };
        for (int index = 0; index < expectedBodies.size(); ++index) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expectedBodies.at(index)) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "unexpected recovery bootstrap request %1").arg(index);
                }
                return;
            }
            if (index == 0) {
                const uint64_t value = 1;
                if (::write(requestSeenFd, &value, sizeof(value)) !=
                    static_cast<ssize_t>(sizeof(value))) {
                    peerError = QStringLiteral(
                        "failed to signal blocked recovery request");
                    return;
                }
                pollfd descriptor{};
                descriptor.fd = cancellationIssuedFd;
                descriptor.events = POLLIN;
                if (::poll(&descriptor, 1, kPeerTimeoutMs) != 1) {
                    peerError = QStringLiteral(
                        "user cancellation was not issued in time");
                    return;
                }
                uint64_t observedValue = 0;
                if (::read(cancellationIssuedFd, &observedValue,
                           sizeof(observedValue)) !=
                    static_cast<ssize_t>(sizeof(observedValue))) {
                    peerError = QStringLiteral(
                        "failed to consume user-cancellation signal");
                    return;
                }
            }

            auto response = baseResponse(request);
            if (index == 0) {
                auto *deviceInfo = response.mutable_device_information();
                deviceInfo->set_product_name("PANORAMA SE");
                deviceInfo->set_firmware_version("test-firmware");
                deviceInfo->set_serial_number("test-serial");
            } else if (index == 1) {
                response.mutable_system_configuration();
            } else {
                response.mutable_device_authentication()->set_auth("test-auth");
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }

        panorama::wire::v1::Request sessionRequest;
        if (!readRequest(sockets[1], &sessionRequest, &peerError) ||
            sessionRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral(
                    "recovery did not reach display-session activation");
            }
            return;
        }
        auto sessionResponse = baseResponse(sessionRequest);
        sessionResponse.mutable_acknowledgement();
        writeResponse(sockets[1], sessionResponse, &peerError);
    });

    constexpr quint64 generation = 39;
    const QString endpoint = QStringLiteral("test-endpoint");
    QThread workerThread;
    auto *worker = new DeviceWorker;
    worker->moveToThread(&workerThread);
    connect(&workerThread, &QThread::finished,
            worker, &QObject::deleteLater);
    workerThread.start();
    QVERIFY(QMetaObject::invokeMethod(
        worker,
        [worker, sockets, endpoint]() {
            worker->updatePrinterGenerationGate(generation, true);
            worker->configurePrinterDevice(
                endpoint, QStringLiteral("test-serial"), generation);
            worker->adoptPrinterFileDescriptorForTesting(sockets[0], endpoint);
        },
        Qt::BlockingQueuedConnection));

    QSignalSpy sessionStartedSpy(worker, &DeviceWorker::printerSessionStarted);
    QSignalSpy operationErrorSpy(worker, &DeviceWorker::printerOperationError);
    QVERIFY(QMetaObject::invokeMethod(
        worker, "startPrinterDisplaySession", Qt::QueuedConnection,
        Q_ARG(QString, endpoint), Q_ARG(quint64, generation)));

    pollfd requestDescriptor{};
    requestDescriptor.fd = requestSeenFd;
    requestDescriptor.events = POLLIN;
    QCOMPARE(::poll(&requestDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t requestValue = 0;
    QCOMPARE(::read(requestSeenFd, &requestValue, sizeof(requestValue)),
             static_cast<ssize_t>(sizeof(requestValue)));
    worker->cancelPrinterOperation(QStringLiteral("cancelled-upload"));
    const uint64_t cancellationValue = 1;
    QCOMPARE(::write(cancellationIssuedFd, &cancellationValue,
                     sizeof(cancellationValue)),
             static_cast<ssize_t>(sizeof(cancellationValue)));

    QTRY_COMPARE_WITH_TIMEOUT(sessionStartedSpy.count(), 1,
                              kPeerTimeoutMs * 3);
    QCOMPARE(operationErrorSpy.count(), 0);
    QVERIFY(QMetaObject::invokeMethod(
        worker, "clearPrinterDevice", Qt::BlockingQueuedConnection,
        Q_ARG(quint64, generation)));
    workerThread.quit();
    QVERIFY(workerThread.wait(kPeerTimeoutMs));

    peer.join();
    ::close(requestSeenFd);
    ::close(cancellationIssuedFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::typedFileListTransaction() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() != panorama::wire::v1::Request::kMediaCatalogQuery) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("unexpected file-list request");
            }
            return;
        }
        auto response = baseResponse(request);
        auto *userFile = response.mutable_media_catalog()->add_media_file_list();
        userFile->set_file_path("/userdata/user/custom.png");
        userFile->set_file_ext(".h264_2240x1080");
        userFile->set_file_size(1234);
        auto *preset = response.mutable_media_catalog()->add_preset_file_list();
        preset->set_file_path("/userdata/default/default_01.mp4.h264_2240x1080");
        preset->set_file_size(5678);
        preset->set_read_only(true);
        writeResponse(sockets[1], response, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    const PrinterProtocol::OperationContext context;
    const auto result = protocol.readMediaList(QStringLiteral("test-endpoint"), context);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.files.size(), 2);
    QCOMPARE(result.files.at(0).name, QStringLiteral("custom.png.h264_2240x1080"));
    QCOMPARE(result.files.at(0).size, quint32(1234));
    QVERIFY(!result.files.at(0).readOnly);
    QCOMPARE(static_cast<int>(result.files.at(0).source),
             static_cast<int>(PrinterProtocol::MediaSource::User));
    QCOMPARE(result.files.at(1).name,
             QStringLiteral("default_01.mp4.h264_2240x1080"));
    QCOMPARE(result.files.at(1).size, quint32(5678));
    QVERIFY(result.files.at(1).readOnly);
    QCOMPARE(static_cast<int>(result.files.at(1).source),
             static_cast<int>(PrinterProtocol::MediaSource::Preset));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteUserMediaLostAckReconcilesWithoutReplay() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const QString target =
        QStringLiteral("delete-me.mp4.h264_2240x1080");
    QString peerError;
    int removeRequests = 0;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral("missing delete preflight FileList");
            return;
        }
        auto listResponse = baseResponse(request);
        auto *file = listResponse.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/delete-me.mp4");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(6125);
        if (!writeResponse(sockets[1], listResponse, &peerError)) {
            return;
        }

        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("missing delete UserConfig preflight");
            return;
        }
        auto configResponse = baseResponse(request);
        configResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("other.mp4.h264_2240x1080");
        if (!writeResponse(sockets[1], configResponse, &peerError)) {
            return;
        }

        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kFileRemoval ||
            request.file_removal().file_name() != target.toStdString() ||
            request.file_removal().file_type() != "media") {
            peerError = QStringLiteral("unexpected FileRemove contract");
            return;
        }
        ++removeRequests;
        // Deliberately omit the optional ACK. The next request must be a
        // read-only reconciliation, never a second FileRemove.
        if (!readRequest(sockets[1], &request, &peerError, 1000) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral(
                "missing FileList reconciliation after optional ACK timeout");
            return;
        }
        auto absentResponse = baseResponse(request);
        absentResponse.mutable_media_catalog();
        writeResponse(sockets[1], absentResponse, &peerError);
    });

    PrinterProtocol protocol(60);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    int dispatchCount = 0;
    const auto beforeDispatch =
        [&dispatchCount](int, const PrinterProtocol::MediaFile &,
                         QString *) {
            ++dispatchCount;
            return true;
        };
    const auto result = protocol.removeUserMedia(
        QStringLiteral("test-endpoint"), QStringList{target},
        beforeDispatch, {}, PrinterProtocol::OperationContext{}, false);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.outcome, PrinterProtocol::MutationOutcome::Succeeded);
    QVERIFY(!result.commandAcknowledged);
    QCOMPARE(result.deletedNames, QStringList{target});
    QCOMPARE(dispatchCount, 1);
    peer.join();
    ::close(sockets[1]);
    QCOMPARE(removeRequests, 1);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteUserMediaAcceptsHeaderOnlySuccess() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const QString target =
        QStringLiteral("header-only.mp4.h264_2240x1080");
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral("missing header-only preflight");
            return;
        }
        auto listResponse = baseResponse(request);
        auto *file = listResponse.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/header-only.mp4");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(5000);
        if (!writeResponse(sockets[1], listResponse, &peerError)) {
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("missing header-only config");
            return;
        }
        auto configResponse = baseResponse(request);
        configResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("other.mp4.h264_2240x1080");
        if (!writeResponse(sockets[1], configResponse, &peerError)) {
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kFileRemoval) {
            peerError = QStringLiteral("missing header-only FileRemove");
            return;
        }
        const auto headerOnlyResponse = baseResponse(request);
        if (!writeResponse(sockets[1], headerOnlyResponse, &peerError)) {
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral("missing header-only reconciliation");
            return;
        }
        auto absentResponse = baseResponse(request);
        absentResponse.mutable_media_catalog();
        writeResponse(sockets[1], absentResponse, &peerError);
    });

    PrinterProtocol protocol(300);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const auto result = protocol.removeUserMedia(
        QStringLiteral("test-endpoint"), QStringList{target},
        [](int, const PrinterProtocol::MediaFile &, QString *) {
            return true;
        },
        {}, PrinterProtocol::OperationContext{}, false);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.commandAcknowledged);
    QCOMPARE(result.deletedNames, QStringList{target});
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteUserMediaRejectsReferencedFileBeforeDispatch() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const QString target =
        QStringLiteral("active.mp4.h264_2240x1080");
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral("missing referenced-file FileList");
            return;
        }
        auto listResponse = baseResponse(request);
        auto *file = listResponse.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/active.mp4");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(4000);
        if (!writeResponse(sockets[1], listResponse, &peerError)) {
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("missing referenced-file UserConfig");
            return;
        }
        auto configResponse = baseResponse(request);
        configResponse.mutable_user_configuration()->mutable_poweron_config()
            ->set_media_file(target.toStdString());
        if (!writeResponse(sockets[1], configResponse, &peerError)) {
            return;
        }
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int polled = ::poll(&descriptor, 1, 150);
        if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) > 0) {
                peerError = QStringLiteral(
                    "FileRemove was sent for referenced media");
            }
        }
    });

    PrinterProtocol protocol(300);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    int dispatchCount = 0;
    const auto result = protocol.removeUserMedia(
        QStringLiteral("test-endpoint"), QStringList{target},
        [&dispatchCount](int, const PrinterProtocol::MediaFile &, QString *) {
            ++dispatchCount;
            return true;
        },
        {}, PrinterProtocol::OperationContext{}, false);
    QVERIFY(!result.success);
    QCOMPARE(result.outcome, PrinterProtocol::MutationOutcome::NotStarted);
    QVERIFY(result.error.contains(QStringLiteral("referenced"),
                                  Qt::CaseInsensitive));
    QCOMPARE(dispatchCount, 0);
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteUserMediaRetainsUnknownAfterBoundedReconciliation() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const QString target =
        QStringLiteral("slow-delete.mp4.h264_2240x1080");
    QString peerError;
    int removeRequests = 0;
    std::thread peer([&]() {
        const auto respondPresent = [&](const panorama::wire::v1::Request &request) {
            auto response = baseResponse(request);
            auto *file = response.mutable_media_catalog()->add_media_file_list();
            file->set_file_path("/userdata/user/slow-delete.mp4");
            file->set_file_ext(".h264_2240x1080");
            file->set_file_size(7000);
            return writeResponse(sockets[1], response, &peerError);
        };
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery ||
            !respondPresent(request)) {
            if (peerError.isEmpty()) {
                peerError = QStringLiteral("missing slow-delete preflight");
            }
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("missing slow-delete config");
            return;
        }
        auto configResponse = baseResponse(request);
        configResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("other.mp4.h264_2240x1080");
        if (!writeResponse(sockets[1], configResponse, &peerError)) {
            return;
        }
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kFileRemoval) {
            peerError = QStringLiteral("missing slow-delete FileRemove");
            return;
        }
        ++removeRequests;
        auto removeResponse = baseResponse(request);
        removeResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], removeResponse, &peerError)) {
            return;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() !=
                    panorama::wire::v1::Request::kMediaCatalogQuery ||
                !respondPresent(request)) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "missing bounded reconciliation read");
                }
                return;
            }
        }
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int polled = ::poll(&descriptor, 1, 150);
        if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) > 0) {
                peerError = QStringLiteral(
                    "FileRemove or unbounded FileList was replayed");
            }
        }
    });

    PrinterProtocol protocol(300);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const auto result = protocol.removeUserMedia(
        QStringLiteral("test-endpoint"), QStringList{target},
        [](int, const PrinterProtocol::MediaFile &, QString *) {
            return true;
        },
        {}, PrinterProtocol::OperationContext{}, false);
    QVERIFY(!result.success);
    QCOMPARE(result.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    QVERIFY(result.commandAcknowledged);
    QVERIFY(result.error.contains(QStringLiteral("will not be repeated"),
                                  Qt::CaseInsensitive));
    peer.join();
    ::close(sockets[1]);
    QCOMPARE(removeRequests, 1);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteReconcileOnlyNeverDispatchesFileRemove() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const QString target =
        QStringLiteral("pending.mp4.h264_2240x1080");
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kMediaCatalogQuery) {
            peerError = QStringLiteral("reconcile-only did not read FileList");
            return;
        }
        auto response = baseResponse(request);
        auto *file = response.mutable_media_catalog()->add_media_file_list();
        file->set_file_path("/userdata/user/pending.mp4");
        file->set_file_ext(".h264_2240x1080");
        file->set_file_size(8000);
        if (!writeResponse(sockets[1], response, &peerError)) {
            return;
        }
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int polled = ::poll(&descriptor, 1, 150);
        if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) > 0) {
                peerError = QStringLiteral(
                    "reconcile-only path sent a mutation");
            }
        }
    });

    PrinterProtocol protocol(300);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    const auto result = protocol.removeUserMedia(
        QStringLiteral("test-endpoint"), QStringList{target}, {}, {},
        PrinterProtocol::OperationContext{}, true);
    QVERIFY(!result.success);
    QCOMPARE(result.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::deleteIntentSurvivesRestartAndOnlyReconcilesSameDevice() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sysRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("sys"));
    const QString devRoot =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("dev"));
    QVERIFY(createUsbDevice(sysRoot, QStringLiteral("1-1"), "1021"));
    QVERIFY(createPrinterEndpoint(sysRoot, devRoot, QStringLiteral("1-1"),
                                  QStringLiteral("lp0")));
    const QString target =
        QStringLiteral("journal.mp4.h264_2240x1080");
    const QString operationId =
        QStringLiteral("17171717-1717-4717-8717-171717171717");
    QString deviceIdentity;

    {
        std::unique_ptr<DeviceManager> manager(
            DeviceManager::createForTesting(sysRoot, devRoot));
        manager->setAutoConnectModeForTesting(true);
        manager->rescanPrinterForTesting();
        manager->printerDisplaySessionActive_ = true;
        deviceIdentity = manager->printerDeviceSerial_;
        QVERIFY(!deviceIdentity.isEmpty());
        PrinterProtocol::MediaFile media;
        media.name = target;
        media.size = 9000;
        media.source = PrinterProtocol::MediaSource::User;
        media.readOnly = false;
        manager->updateMediaCatalog({media});
        QObject::disconnect(manager.get(),
                            &DeviceManager::requestPrinterDeleteMedia,
                            manager->worker_,
                            &DeviceWorker::deletePrinterMedia);
        QSignalSpy deleteRequestSpy(
            manager.get(), &DeviceManager::requestPrinterDeleteMedia);
        QCOMPARE(manager->queueDeleteMediaOperation(operationId,
                                                    QStringList{target}),
                 operationId);
        QCOMPARE(deleteRequestSpy.count(), 1);
        QCOMPARE(deleteRequestSpy.first().at(4).toBool(), false);
        QString intentError;
        QVERIFY2(manager->writeDeleteIntent(
                     operationId, QStringLiteral("Dispatch"), true, 0,
                     target, {}, &intentError),
                 qPrintable(intentError));
        emit manager->worker_->printerDeleteFinished(
            operationId, QStringList{target}, {}, {}, false,
            PrinterProtocol::MutationOutcome::PartialOrUnknown,
            QStringLiteral("simulated lost reconciliation"),
            manager->printerGeneration_);
        const TryxRuntimeOperationInfo pending =
            manager->operationInfo(operationId);
        QCOMPARE(pending.state, QStringLiteral("RetryAvailable"));
        QCOMPARE(pending.retryMode, QStringLiteral("DeleteReconcile"));
        QVERIFY(QFileInfo::exists(manager->deleteIntentPath()));
        manager->cancelOperation(operationId);
        QVERIFY(QFileInfo::exists(manager->deleteIntentPath()));
        QCOMPARE(manager->operationInfo(operationId).retryMode,
                 QStringLiteral("DeleteReconcile"));
    }

    std::unique_ptr<DeviceManager> recovered(
        DeviceManager::createForTesting(sysRoot, devRoot));
    recovered->setAutoConnectModeForTesting(true);
    recovered->rescanPrinterForTesting();
    recovered->printerDisplaySessionActive_ = true;
    recovered->loadDeleteIntent();
    QCOMPARE(recovered->pendingDeleteOperationId_, operationId);
    QCOMPARE(recovered->operationInfo(operationId).retryMode,
             QStringLiteral("DeleteReconcile"));
    QObject::disconnect(recovered.get(),
                        &DeviceManager::requestPrinterDeleteMedia,
                        recovered->worker_,
                        &DeviceWorker::deletePrinterMedia);
    QSignalSpy reconciliationSpy(
        recovered.get(), &DeviceManager::requestPrinterDeleteMedia);
    recovered->printerDeviceSerial_ = QStringLiteral("different-device");
    recovered->resumePendingDeleteReconciliation();
    QCOMPARE(reconciliationSpy.count(), 0);
    QVERIFY(QFileInfo::exists(recovered->deleteIntentPath()));

    recovered->printerDeviceSerial_ = deviceIdentity;
    recovered->resumePendingDeleteReconciliation();
    QCOMPARE(reconciliationSpy.count(), 1);
    QCOMPARE(reconciliationSpy.first().at(1).toStringList(),
             QStringList{target});
    QCOMPARE(reconciliationSpy.first().at(4).toBool(), true);
    emit recovered->worker_->printerDeleteFinished(
        operationId, QStringList{target}, QStringList{target}, {}, true,
        PrinterProtocol::MutationOutcome::Succeeded, QString(),
        recovered->printerGeneration_);
    QCOMPARE(recovered->operationInfo(operationId).state,
             QStringLiteral("Succeeded"));
    QVERIFY(!QFileInfo::exists(recovered->deleteIntentPath()));
    QVERIFY(recovered->pendingDeleteOperationId_.isEmpty());
}

void PrinterProtocolTests::passivePrinterWorkerSendsNoFrames() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, 200);
        if (pollResult < 0) {
            peerError = QStringLiteral("passive session poll failed");
            return;
        }
        if (pollResult > 0) {
            char byte = 0;
            const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
            if (received > 0) {
                peerError = QStringLiteral("passive printer worker sent USB bytes");
            }
        }
    });

    constexpr quint64 generation = 40;
    const QString endpoint = QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"), generation);
    worker.adoptPrinterFileDescriptorForTesting(sockets[0], endpoint);
    QVERIFY(!worker.printerSessionActiveForTesting());
    QVERIFY(QMetaObject::invokeMethod(&worker, "sendPrinterKeepalive",
                                      Qt::DirectConnection));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::printerRefreshStartsSessionAndKeepalive() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int firstKeepaliveSeenFd = eventfd(0, EFD_CLOEXEC);
    QVERIFY(firstKeepaliveSeenFd >= 0);

    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(sockets[1], &peerError,
                               &requestBuffer)) {
            return;
        }
        bool activationSeen = false;
        bool pingKeepaliveSeen = false;
        int fileListsSeen = 0;
        for (int requestCount = 0;
             requestCount < 12 &&
             (!activationSeen || fileListsSeen < 2 ||
              !pingKeepaliveSeen);
             ++requestCount) {
            panorama::wire::v1::Request request;
            QString readError;
            if (!readRequest(sockets[1], &request, &readError,
                             kPeerTimeoutMs * 3, &requestBuffer)) {
                peerError = QStringLiteral(
                    "session request read failed after %1 requests "
                    "(activation=%2 lists=%3 keepalive=%4): %5")
                                .arg(requestCount)
                                .arg(activationSeen)
                                .arg(fileListsSeen)
                                .arg(pingKeepaliveSeen)
                                .arg(readError);
                return;
            }

            auto response = baseResponse(request);
            if (request.body_case() ==
                panorama::wire::v1::Request::kOverlayLayout) {
                if (!activationSeen) {
                    if (!request.has_header() ||
                        request.header().version() != 1 ||
                        request.header().track_id() == 0) {
                        peerError = QStringLiteral(
                            "unexpected tracked display-session activation");
                        return;
                    }
                    activationSeen = true;
                } else {
                    peerError = QStringLiteral(
                        "periodic keepalive repeated RunConfig instead of Ping");
                    return;
                }
                response.mutable_acknowledgement();
            } else if (request.body_case() ==
                       panorama::wire::v1::Request::kMediaCatalogQuery) {
                if (!activationSeen || fileListsSeen >= 2) {
                    peerError = QStringLiteral(
                        "unexpected FileList request in display session");
                    return;
                }
                ++fileListsSeen;
                auto *file =
                    response.mutable_media_catalog()->add_media_file_list();
                file->set_file_path("/userdata/user/session-test.png");
                file->set_file_ext(".h264_2240x1080");
                file->set_file_size(1234);
            } else if (request.body_case() ==
                       panorama::wire::v1::Request::kPing) {
                if (!activationSeen || !request.has_header() ||
                    request.header().ByteSizeLong() != 0 ||
                    request.ping().payload() != "hello?") {
                    peerError = QStringLiteral(
                        "periodic keepalive did not match UDB Ping");
                    return;
                }
                pingKeepaliveSeen = true;
                const uint64_t seenValue = 1;
                if (::write(firstKeepaliveSeenFd, &seenValue,
                            sizeof(seenValue)) !=
                    static_cast<ssize_t>(sizeof(seenValue))) {
                    peerError = QStringLiteral(
                        "first keepalive readiness signal failed");
                    return;
                }
                response.mutable_pong()->set_payload(
                    request.ping().payload());
            } else {
                peerError = QStringLiteral("unexpected session request body %1")
                                .arg(static_cast<int>(
                                    request.body_case()));
                return;
            }

            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }
        if (!activationSeen || fileListsSeen != 2 ||
            !pingKeepaliveSeen) {
            peerError = QStringLiteral(
                "display session sequence did not complete");
            return;
        }

        pollfd closeDescriptor{};
        closeDescriptor.fd = sockets[1];
        closeDescriptor.events = POLLIN;
        const int closePoll = ::poll(&closeDescriptor, 1, 100);
        if (closePoll < 0) {
            peerError = QString::fromLocal8Bit(std::strerror(errno));
            return;
        }
        char byte = 0;
        const ssize_t peeked = ::recv(
            sockets[1], &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
        if (peeked == 0) {
            peerError = QStringLiteral(
                "display keepalive unexpectedly closed the persistent transport");
        } else if (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            peerError = QString::fromLocal8Bit(std::strerror(errno));
        }
    });

    constexpr quint64 generation = 41;
    const QString endpoint = QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"), generation);
    worker.adoptPrinterFileDescriptorForTesting(sockets[0], endpoint);
    QSignalSpy listSpy(&worker, &DeviceWorker::printerMediaListReady);
    QSignalSpy errorSpy(&worker, &DeviceWorker::printerMediaListFailed);

    worker.refreshPrinterMediaList(endpoint, QString(), generation);
    worker.refreshPrinterMediaList(endpoint, QString(), generation);
    if (errorSpy.count() != 0 || listSpy.count() != 2) {
        worker.clearPrinterDevice(generation);
        peer.join();
        ::close(firstKeepaliveSeenFd);
        ::close(sockets[1]);
        QFAIL(qPrintable(
            QStringLiteral(
                "refresh session failed: errors=%1 lists=%2 first=%3 second=%4 peer=%5")
                .arg(errorSpy.count())
                .arg(listSpy.count())
                .arg(errorSpy.count() > 0
                         ? errorSpy.at(0).at(1).toString()
                         : QStringLiteral("<none>"))
                .arg(errorSpy.count() > 1
                         ? errorSpy.at(1).at(1).toString()
                         : QStringLiteral("<none>"))
                .arg(peerError)));
    }
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(listSpy.count(), 2);
    const QList<PrinterProtocol::MediaFile> firstFiles =
        qvariant_cast<QList<PrinterProtocol::MediaFile>>(
            listSpy.at(0).at(1));
    QCOMPARE(firstFiles.size(), 1);
    QCOMPARE(firstFiles.first().name,
             QStringLiteral("session-test.png.h264_2240x1080"));
    QVERIFY(worker.printerSessionActiveForTesting());
    QVERIFY(QMetaObject::invokeMethod(&worker, "sendPrinterKeepalive",
                                      Qt::DirectConnection));
    pollfd keepaliveDescriptor{};
    keepaliveDescriptor.fd = firstKeepaliveSeenFd;
    keepaliveDescriptor.events = POLLIN;
    QCOMPARE(::poll(&keepaliveDescriptor, 1, kPeerTimeoutMs), 1);
    uint64_t seenValue = 0;
    QCOMPARE(::read(firstKeepaliveSeenFd, &seenValue, sizeof(seenValue)),
             static_cast<ssize_t>(sizeof(seenValue)));
    QCOMPARE(seenValue, uint64_t(1));

    peer.join();
    ::close(firstKeepaliveSeenFd);
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
runConfigErrorResponseRejectsSessionStart() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(sockets[1], &peerError,
                               &requestBuffer)) {
            return;
        }
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError,
                         kPeerTimeoutMs, &requestBuffer) ||
            request.body_case() != panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral("session failure test did not receive RunConfig");
            return;
        }
        auto response = baseResponse(request);
        response.mutable_error()->set_code(panorama::wire::v1::ProtocolError::FAILURE);
        response.mutable_error()->set_why("session rejected");
        if (!writeResponse(sockets[1], response, &peerError)) {
            return;
        }
    });

    constexpr quint64 generation = 42;
    const QString endpoint = QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"), generation);
    worker.adoptPrinterFileDescriptorForTesting(sockets[0], endpoint);
    QSignalSpy listSpy(&worker, &DeviceWorker::printerMediaListReady);
    QSignalSpy errorSpy(&worker, &DeviceWorker::printerMediaListFailed);

    worker.refreshPrinterMediaList(endpoint, QString(), generation);
    QCOMPARE(listSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!worker.printerSessionActiveForTesting());
    QVERIFY(errorSpy.first().at(1).toString().contains(
        QStringLiteral("session rejected"),
        Qt::CaseInsensitive));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
restoredOverlayWaitsForKeepaliveBeforeSessionReady() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    const QString expectedCpu =
        SystemMonitor::cpuModelName().trimmed();
    QVERIFY(!expectedCpu.isEmpty());
    SystemMonitor modelMonitor;
    const QString expectedGpu =
        modelMonitor.primaryGpuModelName().trimmed();

    QString peerError;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(
                sockets[1], &peerError, &requestBuffer)) {
            return;
        }
        panorama::wire::v1::Request bootstrapRunRequest;
        if (!readRequest(
                sockets[1], &bootstrapRunRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            bootstrapRunRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "restored overlay test did not receive bootstrap RunConfig");
            return;
        }
        if (!bootstrapRunRequest.has_header() ||
            bootstrapRunRequest.header().version() != 1 ||
            bootstrapRunRequest.header().track_id() == 0 ||
            bootstrapRunRequest.overlay_layout().label_groups_size() != 0) {
            peerError = QStringLiteral(
                "bootstrap RunConfig was not an empty tracked activation");
            return;
        }
        auto bootstrapRunResponse =
            baseResponse(bootstrapRunRequest);
        bootstrapRunResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], bootstrapRunResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request keepaliveRequest;
        if (!readRequest(
                sockets[1], &keepaliveRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            keepaliveRequest.body_case() !=
                panorama::wire::v1::Request::kPing ||
            !keepaliveRequest.has_header() ||
            keepaliveRequest.header().ByteSizeLong() != 0 ||
            keepaliveRequest.ping().payload() != "hello?") {
            peerError = QStringLiteral(
                "restored overlay test did not receive the readiness Ping");
            return;
        }
        panorama::wire::v1::Response keepaliveResponse;
        keepaliveResponse.mutable_header();
        keepaliveResponse.mutable_pong()->set_payload("Hey!");
        if (!writeResponse(sockets[1], keepaliveResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request overlayRunRequest;
        if (!readRequest(
                sockets[1], &overlayRunRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            overlayRunRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral(
                "restored overlay test did not receive the full RunConfig");
            return;
        }
        if (!overlayRunRequest.has_header() ||
            overlayRunRequest.header().version() != 1 ||
            overlayRunRequest.header().track_id() == 0 ||
            overlayRunRequest.header().track_id() ==
                bootstrapRunRequest.header().track_id()) {
            peerError = QStringLiteral(
                "restored overlay RunConfig was not a new tracked mutation");
            return;
        }
        const auto &run = overlayRunRequest.overlay_layout();
        const panorama::wire::v1::OverlayGroup *metricGroup =
            nullptr;
        const panorama::wire::v1::OverlayGroup *badgeGroup =
            nullptr;
        for (int index = 0;
             index < run.label_groups_size(); ++index) {
            if (run.label_groups(index).group_id() == 100U) {
                metricGroup = &run.label_groups(index);
            } else if (
                run.label_groups(index).group_id() == 300U) {
                badgeGroup = &run.label_groups(index);
            }
        }
        if (!metricGroup ||
            metricGroup->labels_size() < 3 ||
            !badgeGroup ||
            badgeGroup->labels_size() <
                (expectedGpu.isEmpty() ? 1 : 2) ||
            QString::fromStdString(
                badgeGroup->labels(0).text()).trimmed() != expectedCpu ||
            (!expectedGpu.isEmpty() &&
             QString::fromStdString(
                 badgeGroup->labels(1).text()).trimmed() != expectedGpu)) {
            peerError = QStringLiteral(
                "restored overlay was not hydrated: "
                "metric_labels=%1 badge_labels=%2 cpu=%3 gpu=%4")
                            .arg(
                                metricGroup
                                    ? metricGroup->labels_size()
                                    : -1)
                            .arg(
                                badgeGroup
                                    ? badgeGroup->labels_size()
                                    : -1)
                            .arg(
                                badgeGroup &&
                                        badgeGroup->labels_size() > 0
                                    ? QString::fromStdString(
                                          badgeGroup->labels(0)
                                              .text())
                                    : QStringLiteral("<missing>"))
                            .arg(
                                badgeGroup &&
                                        badgeGroup->labels_size() > 1
                                    ? QString::fromStdString(
                                          badgeGroup->labels(1)
                                              .text())
                                    : QStringLiteral("<missing>"));
            return;
        }
        auto runResponse = baseResponse(overlayRunRequest);
        runResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], runResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request activePing;
        if (!readRequest(
                sockets[1], &activePing, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            activePing.body_case() !=
                panorama::wire::v1::Request::kPing ||
            !activePing.has_header() ||
            activePing.header().ByteSizeLong() != 0) {
            peerError = QStringLiteral(
                "active overlay session did not send Ping first");
            return;
        }
        panorama::wire::v1::Response activePong;
        activePong.mutable_header();
        activePong.mutable_pong()->set_payload("Hey!");
        if (!writeResponse(sockets[1], activePong,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request overlayLease;
        if (!readRequest(
                sockets[1], &overlayLease, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            overlayLease.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout ||
            !overlayLease.has_header() ||
            overlayLease.header().ByteSizeLong() != 0 ||
            overlayLease.overlay_layout().label_groups_size() == 0) {
            peerError = QStringLiteral(
                "active overlay session did not refresh the layout lease");
            return;
        }
        panorama::wire::v1::Response leaseResponse;
        leaseResponse.mutable_header();
        leaseResponse.mutable_acknowledgement();
        writeResponse(sockets[1], leaseResponse,
                      &peerError);
    });

    constexpr quint64 generation = 43;
    const QString endpoint =
        QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"),
        generation);
    worker.adoptPrinterFileDescriptorForTesting(
        sockets[0], endpoint);
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    overlay.left.badges = {
        QStringLiteral("CPU Badge")};
    if (!expectedGpu.isEmpty()) {
        overlay.left.badges.append(
            QStringLiteral("GPU Badge"));
    }
    worker.restorePrinterOverlay(overlay, generation);
    QSignalSpy startedSpy(
        &worker, &DeviceWorker::printerSessionStarted);
    QSignalSpy lostSpy(
        &worker, &DeviceWorker::printerSessionLost);
    QSignalSpy errorSpy(
        &worker, &DeviceWorker::printerOperationError);
    QSignalSpy readySpy(
        &worker, &DeviceWorker::printerTransportReady);

    worker.startPrinterDisplaySession(endpoint, generation);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::
                 AwaitingOverlayActivation);
    QVERIFY(worker.printerOverlayActivationPending_);
    QVERIFY(!worker.printerSessionActiveForTesting());
    QVERIFY(!worker.printerMetricsTimer_->isActive());
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(lostSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Active);
    QVERIFY(!worker.printerOverlayActivationPending_);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(lostSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(readySpy.count(), 3);
    QVERIFY(worker.printerSessionActiveForTesting());
    QVERIFY(worker.printerMetricsTimer_->isActive());
    QVERIFY(!worker.printerRecoveryTimer_->isActive());
}

void PrinterProtocolTests::
restoredOverlayFailureBecomesLostWithoutReplay() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    int overlayRunCount = 0;
    std::thread peer([&]() {
        QByteArray requestBuffer;
        if (!serveUdbBootstrap(
                sockets[1], &peerError, &requestBuffer)) {
            return;
        }

        panorama::wire::v1::Request bootstrapRunRequest;
        if (!readRequest(
                sockets[1], &bootstrapRunRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            bootstrapRunRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout ||
            bootstrapRunRequest.overlay_layout()
                    .label_groups_size() != 0) {
            peerError = QStringLiteral(
                "overlay failure test did not receive empty bootstrap RunConfig");
            return;
        }
        auto bootstrapResponse =
            baseResponse(bootstrapRunRequest);
        bootstrapResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], bootstrapResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request keepaliveRequest;
        if (!readRequest(
                sockets[1], &keepaliveRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            keepaliveRequest.body_case() !=
                panorama::wire::v1::Request::kPing) {
            peerError = QStringLiteral(
                "overlay failure test did not receive readiness Ping");
            return;
        }
        panorama::wire::v1::Response keepaliveResponse;
        keepaliveResponse.mutable_header();
        keepaliveResponse.mutable_pong()->set_payload("Hey!");
        if (!writeResponse(sockets[1], keepaliveResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request overlayRunRequest;
        if (!readRequest(
                sockets[1], &overlayRunRequest, &peerError,
                kPeerTimeoutMs, &requestBuffer) ||
            overlayRunRequest.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout ||
            overlayRunRequest.overlay_layout()
                    .label_groups_size() == 0) {
            peerError = QStringLiteral(
                "overlay failure test did not receive full overlay RunConfig");
            return;
        }
        ++overlayRunCount;
        auto rejectedResponse =
            baseResponse(overlayRunRequest);
        rejectedResponse.mutable_error()->set_code(
            panorama::wire::v1::ProtocolError::FAILURE);
        rejectedResponse.mutable_error()->set_why(
            "overlay rejected");
        if (!writeResponse(sockets[1], rejectedResponse,
                           &peerError)) {
            return;
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult =
            ::poll(&descriptor, 1, 200);
        if (pollResult < 0) {
            peerError = QStringLiteral(
                "overlay replay check poll failed");
            return;
        }
        if (pollResult > 0 &&
            (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            const ssize_t received =
                ::recv(sockets[1], &byte, sizeof(byte),
                       MSG_DONTWAIT);
            if (received > 0) {
                peerError = QStringLiteral(
                    "overlay RunConfig was replayed after rejection");
            }
        }
    });

    constexpr quint64 generation = 44;
    const QString endpoint =
        QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"),
        generation);
    worker.adoptPrinterFileDescriptorForTesting(
        sockets[0], endpoint);
    PrinterProtocol::PaseOverlayConfig overlay;
    overlay.left.metrics = {
        QStringLiteral("CPU Temperature")};
    overlay.left.badges = {
        QStringLiteral("CPU Badge")};
    worker.restorePrinterOverlay(overlay, generation);

    QSignalSpy startedSpy(
        &worker, &DeviceWorker::printerSessionStarted);
    QSignalSpy stoppedSpy(
        &worker, &DeviceWorker::printerSessionStopped);
    QSignalSpy lostSpy(
        &worker, &DeviceWorker::printerSessionLost);
    QSignalSpy errorSpy(
        &worker, &DeviceWorker::printerOperationError);

    worker.startPrinterDisplaySession(endpoint, generation);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::
                 AwaitingOverlayActivation);
    QCOMPARE(startedSpy.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(overlayRunCount, 1);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Lost);
    QVERIFY(!worker.printerOverlayActivationPending_);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(lostSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!worker.printerKeepaliveTimer_->isActive());
    QVERIFY(!worker.printerMetricsTimer_->isActive());
    QVERIFY(!worker.printerRecoveryTimer_->isActive());
}

void PrinterProtocolTests::
overlayLeaseRejectionBecomesLostWithoutRecovery() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));

    QString peerError;
    int leaseCount = 0;
    std::thread peer([&]() {
        panorama::wire::v1::Request request;
        if (!readRequest(sockets[1], &request, &peerError) ||
            request.body_case() !=
                panorama::wire::v1::Request::kOverlayLayout ||
            !request.has_header() ||
            request.header().ByteSizeLong() != 0 ||
            request.overlay_layout().label_groups_size() == 0) {
            peerError = QStringLiteral(
                "overlay lease rejection test did not receive an untracked layout");
            return;
        }
        ++leaseCount;
        panorama::wire::v1::Response response;
        response.mutable_error()->set_code(
            panorama::wire::v1::ProtocolError::FAILURE);
        response.mutable_error()->set_why(
            "renderer rejected overlay lease");
        writeResponse(sockets[1], response, &peerError);
    });

    constexpr quint64 generation = 45;
    const QString endpoint =
        QStringLiteral("test-endpoint");
    DeviceWorker worker;
    worker.updatePrinterGenerationGate(generation, true);
    worker.configurePrinterDevice(
        endpoint, QStringLiteral("test-serial"),
        generation);
    worker.adoptPrinterFileDescriptorForTesting(
        sockets[0], endpoint);
    worker.printerSessionState_ =
        DeviceWorker::PrinterSessionState::Active;
    worker.printerOverlayConfig_.left.metrics = {
        QStringLiteral("CPU Temperature")};
    worker.printerOverlayLeaseRefreshNext_ = true;
    worker.printerMetricsTimer_->start(10000);
    worker.printerRecoveryTimer_->start(10000);

    QSignalSpy lostSpy(
        &worker, &DeviceWorker::printerSessionLost);
    QSignalSpy errorSpy(
        &worker, &DeviceWorker::printerOperationError);

    QVERIFY(QMetaObject::invokeMethod(
        &worker, "sendPrinterKeepalive",
        Qt::DirectConnection));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QCOMPARE(leaseCount, 1);
    QCOMPARE(worker.printerSessionState_,
             DeviceWorker::PrinterSessionState::Lost);
    QCOMPARE(lostSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!worker.printerOverlayLeaseRefreshNext_);
    QVERIFY(!worker.printerKeepaliveTimer_->isActive());
    QVERIFY(!worker.printerMetricsTimer_->isActive());
    QVERIFY(!worker.printerRecoveryTimer_->isActive());
}

void PrinterProtocolTests::applyMediaPreservesUnknownFields() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("unexpected get-user-config request");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        auto *config = getResponse.mutable_user_configuration();
        config->mutable_poweron_config()->set_media_file("keep-poweron.h264");
        config->mutable_display_config()->set_backlight_brightness(55);
        config->mutable_filter_config()->set_alpha(73);
        config->mutable_work_config()->set_media_mode(
            panorama::wire::v1::WorkConfiguration::MEDIA_DUAL);
        config->mutable_work_config()->set_loop_mode(
            panorama::wire::v1::WorkConfiguration::LOOP_ALL);
        config->mutable_work_config()->set_single_mode_media_file("old.h264");
        config->GetReflection()->MutableUnknownFields(config)->AddVarint(99, 123456);
        config->mutable_work_config()->GetReflection()
            ->MutableUnknownFields(config->mutable_work_config())
            ->AddLengthDelimited(77, "nested-unknown");
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request applyRequest;
        if (!readRequest(sockets[1], &applyRequest, &peerError) ||
            applyRequest.body_case() != panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral("unexpected user-config apply request");
            return;
        }
        const auto &applied = applyRequest.user_configuration();
        if (applied.work_config().media_mode() !=
                panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE ||
            applied.work_config().loop_mode() !=
                panorama::wire::v1::WorkConfiguration::LOOP_SINGLE ||
            applied.work_config().single_mode_media_file() != "new.h264" ||
            applied.poweron_config().media_file() != "keep-poweron.h264" ||
            applied.display_config().backlight_brightness() != 55 ||
            applied.filter_config().alpha() != 73 ||
            !hasUnknownField(applied.GetReflection()->GetUnknownFields(applied), 99) ||
            !hasUnknownField(applied.work_config().GetReflection()->GetUnknownFields(
                                 applied.work_config()),
                             77)) {
            peerError = QStringLiteral("user config was not mutated in place");
            return;
        }
        auto applyResponse = baseResponse(applyRequest);
        applyResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], applyResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest, &peerError) ||
            runRequest.body_case() != panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral("unexpected run-config request");
            return;
        }
        auto runResponse = baseResponse(runRequest);
        runResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], runResponse,
                           &peerError)) {
            return;
        }

        panorama::wire::v1::Request readbackRequest;
        if (!readRequest(sockets[1], &readbackRequest,
                         &peerError) ||
            readbackRequest.body_case() !=
                panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral(
                "unexpected user-config readback request");
            return;
        }
        auto readbackResponse =
            baseResponse(readbackRequest);
        *readbackResponse.mutable_user_configuration() =
            applyRequest.user_configuration();
        writeResponse(
            sockets[1], readbackResponse, &peerError);
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY2(protocol.applyPresetMedia(QStringLiteral("test-endpoint"),
                                       QStringLiteral("new.h264"), -1,
                                       &error, context),
             qPrintable(error));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::rejectedApplyDoesNotSendRunConfig() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("unexpected get-user-config request");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        getResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("old.h264");
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request applyRequest;
        if (!readRequest(sockets[1], &applyRequest, &peerError) ||
            applyRequest.body_case() != panorama::wire::v1::Request::kUserConfiguration) {
            peerError = QStringLiteral("unexpected user-config apply request");
            return;
        }
        auto applyResponse = baseResponse(applyRequest);
        applyResponse.mutable_error()->set_code(panorama::wire::v1::ProtocolError::FAILURE);
        applyResponse.mutable_error()->set_why("rejected apply");
        if (!writeResponse(sockets[1], applyResponse, &peerError)) {
            return;
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, 100);
        if (pollResult > 0 && (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
            if (received > 0) {
                peerError = QStringLiteral("run-config was sent after rejected apply");
            }
        } else if (pollResult < 0 && errno != EINTR) {
            peerError = QStringLiteral("failed to verify rejected apply boundary");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    QVERIFY(!protocol.applyPresetMedia(QStringLiteral("test-endpoint"),
                                       QStringLiteral("new.h264"), -1,
                                       &error, context));
    QVERIFY(error.contains(QStringLiteral("rejected apply")));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::userConfigOutcomeUnknownAfterFullSendIsPartial_data() {
    QTest::addColumn<bool>("disconnectAfterSend");
    QTest::newRow("user-config-cancelled-after-send") << false;
    QTest::newRow("user-config-disconnected-after-send") << true;
}

void PrinterProtocolTests::userConfigOutcomeUnknownAfterFullSendIsPartial() {
    QFETCH(bool, disconnectAfterSend);
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int cancellationFd = disconnectAfterSend
        ? -1
        : ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    QVERIFY(disconnectAfterSend || cancellationFd >= 0);

    QString peerError;
    bool peerClosedEndpoint = false;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("unexpected get-user-config request");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        getResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("old.h264");
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request applyRequest;
        if (!readRequest(sockets[1], &applyRequest, &peerError) ||
            applyRequest.body_case() != panorama::wire::v1::Request::kUserConfiguration ||
            applyRequest.user_configuration().work_config().single_mode_media_file() !=
                "new.h264") {
            peerError = QStringLiteral("user configuration was not fully sent");
            return;
        }

        if (disconnectAfterSend) {
            ::close(sockets[1]);
            peerClosedEndpoint = true;
            return;
        } else {
            const uint64_t value = 1;
            if (::write(cancellationFd, &value, sizeof(value)) !=
                static_cast<ssize_t>(sizeof(value))) {
                peerError = QStringLiteral("failed to signal user-config cancellation");
                return;
            }
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = QStringLiteral("uncertain user-config session stayed open");
            return;
        }
        char byte = 0;
        const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
        if (received != 0) {
            peerError = received > 0
                ? QStringLiteral("unexpected write after uncertain user-config outcome")
                : QStringLiteral("failed to confirm uncertain session close");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationFd;
    QString error;
    QElapsedTimer timer;
    timer.start();
    PrinterProtocol::MutationDetails mutation;
    const bool success = protocol.applyPresetMedia(
        QStringLiteral("test-endpoint"), QStringLiteral("new.h264"), -1,
        &error, context, &mutation);
    const qint64 elapsedMs = timer.elapsed();
    peer.join();
    if (!peerClosedEndpoint) {
        ::close(sockets[1]);
    }
    if (cancellationFd >= 0) {
        ::close(cancellationFd);
    }

    QVERIFY(!success);
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    QCOMPARE(mutation.stage, QStringLiteral("WritingConfig"));
    QVERIFY(error.contains(QStringLiteral("fully sent"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("not confirmed"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("rollback"), Qt::CaseInsensitive));
    if (disconnectAfterSend) {
        QVERIFY(error.contains(QStringLiteral("disconnect"), Qt::CaseInsensitive));
    } else {
        QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    }
    QVERIFY(elapsedMs < 500);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::runConfigFailureAfterAcceptedUserConfigIsPartial_data() {
    QTest::addColumn<bool>("cancelAfterRunSend");
    QTest::newRow("run-out-disconnected") << false;
    QTest::newRow("run-cancelled-after-send") << true;
}

void PrinterProtocolTests::runConfigFailureAfterAcceptedUserConfigIsPartial() {
    QFETCH(bool, cancelAfterRunSend);
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int cancellationFd = cancelAfterRunSend
        ? ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)
        : -1;
    QVERIFY(!cancelAfterRunSend || cancellationFd >= 0);

    QString peerError;
    bool peerClosedEndpoint = false;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("unexpected get-user-config request");
            return;
        }
        auto getResponse = baseResponse(getRequest);
        getResponse.mutable_user_configuration()->mutable_work_config()
            ->set_single_mode_media_file("old.h264");
        if (!writeResponse(sockets[1], getResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request applyRequest;
        if (!readRequest(sockets[1], &applyRequest, &peerError) ||
            applyRequest.body_case() != panorama::wire::v1::Request::kUserConfiguration ||
            applyRequest.user_configuration().work_config().single_mode_media_file() !=
                "new.h264") {
            peerError = QStringLiteral("unexpected user-config apply request");
            return;
        }
        auto applyResponse = baseResponse(applyRequest);
        applyResponse.mutable_acknowledgement();
        if (!writeResponse(sockets[1], applyResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request runRequest;
        if (!readRequest(sockets[1], &runRequest, &peerError) ||
            runRequest.body_case() != panorama::wire::v1::Request::kOverlayLayout) {
            peerError = QStringLiteral("unexpected run-config request");
            return;
        }
        if (!cancelAfterRunSend) {
            ::close(sockets[1]);
            peerClosedEndpoint = true;
            return;
        }
        const uint64_t value = 1;
        if (::write(cancellationFd, &value, sizeof(value)) !=
            static_cast<ssize_t>(sizeof(value))) {
            peerError = QStringLiteral(
                "failed to signal post-send cancellation");
            return;
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.cancellationFd = cancellationFd;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    const bool applied = protocol.applyPresetMedia(
        QStringLiteral("test-endpoint"),
        QStringLiteral("new.h264"), -1,
        &error, context, &mutation);
    peer.join();
    if (cancellationFd >= 0) {
        ::close(cancellationFd);
    }
    if (!peerClosedEndpoint) {
        ::close(sockets[1]);
    }
    QVERIFY(!applied);
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    if (cancelAfterRunSend) {
        QCOMPARE(mutation.stage,
                 QStringLiteral("VerifyingConfig"));
        QVERIFY(error.contains(
            QStringLiteral("activated"),
            Qt::CaseInsensitive));
        QVERIFY(error.contains(
            QStringLiteral("readback"),
            Qt::CaseInsensitive));
        QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    } else {
        QCOMPARE(mutation.stage,
                 QStringLiteral("ActivatingConfig"));
        QVERIFY(error.contains(
            QStringLiteral("accepted"),
            Qt::CaseInsensitive));
        QVERIFY(error.contains(
            QStringLiteral("rollback"),
            Qt::CaseInsensitive));
        QVERIFY(error.contains(
            QStringLiteral("disconnect"),
            Qt::CaseInsensitive));
    }
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::incompleteUserConfigIsNotWritten_data() {
    QTest::addColumn<bool>("brightnessOnly");
    QTest::newRow("missing-work-config") << false;
    QTest::newRow("missing-display-config") << true;
}

void PrinterProtocolTests::incompleteUserConfigIsNotWritten() {
    QFETCH(bool, brightnessOnly);
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request getRequest;
        if (!readRequest(sockets[1], &getRequest, &peerError) ||
            getRequest.body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
            peerError = QStringLiteral("unexpected get-user-config request");
            return;
        }
        auto response = baseResponse(getRequest);
        response.mutable_user_configuration();
        if (!writeResponse(sockets[1], response, &peerError)) {
            return;
        }
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, 100);
        if (pollResult > 0 && (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
            if (received > 0) {
                peerError = QStringLiteral("synthetic user configuration was written");
            }
        } else if (pollResult < 0 && errno != EINTR) {
            peerError = QStringLiteral("failed to verify incomplete config boundary");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    const PrinterProtocol::OperationContext context;
    const bool success = brightnessOnly
        ? protocol.setBrightness(QStringLiteral("test-endpoint"), 50, &error, context)
        : protocol.applyPresetMedia(QStringLiteral("test-endpoint"),
                                    QStringLiteral("new.h264"), -1, &error, context);
    QVERIFY(!success);
    QVERIFY(error.contains(QStringLiteral("synthetic"), Qt::CaseInsensitive));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::
paseStandbyMutationIsRejectedBeforeUsb() {
    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));
    QString peerError;
    std::thread peer([&]() {
        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult =
            ::poll(&descriptor, 1, 100);
        if (pollResult > 0 &&
            (descriptor.revents & POLLIN) != 0) {
            char byte = 0;
            const ssize_t received = ::recv(
                sockets[1], &byte, sizeof(byte),
                MSG_DONTWAIT);
            if (received > 0) {
                peerError = QStringLiteral(
                    "standby mutation reached USB");
            }
        } else if (pollResult < 0 && errno != EINTR) {
            peerError = QStringLiteral(
                "standby boundary poll failed");
        }
    });

    PrinterProtocol protocol(500);
    const QString endpoint =
        QStringLiteral(
            "/dev/usb/lp-pase-standby-rejected");
    protocol.adoptFileDescriptorForTesting(
        sockets[0], endpoint);
    PrinterProtocol::PaseApplyConfig config;
    config.display.standbyPresent = true;
    config.display.standbyEnabled = false;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.applyPaseConfiguration(
        endpoint, config, &error,
        PrinterProtocol::OperationContext{},
        &mutation));
    QCOMPARE(
        mutation.outcome,
        PrinterProtocol::MutationOutcome::Rejected);
    QVERIFY(error.contains(
        QStringLiteral("standby"),
        Qt::CaseInsensitive));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::unsafeMediaNamesAreRejected() {
    const QStringList unsafeNames = {
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral(".hidden.h264"),
        QStringLiteral("folder/file.h264"),
        QStringLiteral("name with spaces.h264")
    };
    const PrinterProtocol::OperationContext context;
    for (const QString &name : unsafeNames) {
        PrinterProtocol protocol(100);
        QString error;
        QVERIFY2(!protocol.applyPresetMedia(QStringLiteral("unused"), name, -1,
                                            &error, context),
                 qPrintable(name));
        QVERIFY2(error.contains(QStringLiteral("not supported"), Qt::CaseInsensitive),
                 qPrintable(error));
    }
}

void PrinterProtocolTests::uploadRejectsSymlinkAndHashMismatchBeforeUsb() {
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sourcePath =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("source.h264"));
    const QByteArray sourceBytes(4096, '\x6a');
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(source.write(sourceBytes),
             static_cast<qint64>(sourceBytes.size()));
    source.close();
    const QString symlinkPath =
        QDir(temporaryDirectory.path()).filePath(QStringLiteral("link.h264"));
    const QByteArray encodedSource = QFile::encodeName(sourcePath);
    const QByteArray encodedLink = QFile::encodeName(symlinkPath);
    QCOMPARE(::symlink(encodedSource.constData(), encodedLink.constData()), 0);

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    PrinterProtocol protocol;
    const QString devicePath = QStringLiteral("/dev/usb/lp-pase-safe-open");
    protocol.adoptFileDescriptorForTesting(sockets[0], devicePath);

    QString uploadedName;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.uploadMedia(
        devicePath, symlinkPath,
        QStringLiteral("safe.mp4.h264_2240x1080"), &uploadedName, &error,
        {}, PrinterProtocol::OperationContext{}, &mutation,
        QString::fromLatin1(
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256)
                .toHex())));
    QVERIFY(error.contains(QStringLiteral("does not exist"),
                           Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome, PrinterProtocol::MutationOutcome::NotStarted);
    QCOMPARE(mutation.stage, QStringLiteral("Validating"));

    pollfd descriptor{};
    descriptor.fd = sockets[1];
    descriptor.events = POLLIN;
    QCOMPARE(::poll(&descriptor, 1, 50), 0);

    error.clear();
    mutation = {};
    QVERIFY(!protocol.uploadMedia(
        devicePath, sourcePath,
        QStringLiteral("safe.mp4.h264_2240x1080"), &uploadedName, &error,
        {}, PrinterProtocol::OperationContext{}, &mutation,
        QString(64, QLatin1Char('0'))));
    QVERIFY(error.contains(QStringLiteral("changed"), Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome, PrinterProtocol::MutationOutcome::NotStarted);
    QCOMPARE(mutation.stage, QStringLiteral("Validating"));
    descriptor.revents = 0;
    QCOMPARE(::poll(&descriptor, 1, 50), 0);
    ::close(sockets[1]);
}

void PrinterProtocolTests::duplexInputReceivesAckDuringOutput() {
    QList<PrinterProtocol::DuplexTestEvent> events;
    PrinterProtocol::DuplexTestEvent input;
    input.direction = PrinterProtocol::DuplexTestDirection::Input;
    input.payload = QByteArrayLiteral("ack");
    events.append(input);
    PrinterProtocol::DuplexTestEvent output;
    output.direction = PrinterProtocol::DuplexTestDirection::Output;
    events.append(output);

    const PrinterProtocol::DuplexTestResult result =
        PrinterProtocol::runDuplexTransportScenarioForTesting(
            events, QByteArrayLiteral("request"));
    QVERIFY2(result.writeSucceeded, qPrintable(result.error));
    QVERIFY(result.responseReceived);
    QCOMPARE(result.response, QByteArrayLiteral("ack"));
    QCOMPARE(result.inputSubmissions, 1);
    QCOMPARE(result.outputSubmissions, 1);
    QCOMPARE(result.maximumConcurrentInputs, 1);
    QCOMPARE(result.inputCompletions, 1);
    QCOMPARE(result.zeroLengthInputCompletions, 0);
    QCOMPARE(result.inputErrors, 0);
    QCOMPARE(result.inputRearmsDuringOutput, 0);
}

void PrinterProtocolTests::
    duplexZeroLengthInputDefersRearmUntilOutputCompletes() {
    QList<PrinterProtocol::DuplexTestEvent> events;
    PrinterProtocol::DuplexTestEvent emptyInput;
    emptyInput.direction = PrinterProtocol::DuplexTestDirection::Input;
    events.append(emptyInput);
    PrinterProtocol::DuplexTestEvent output;
    output.direction = PrinterProtocol::DuplexTestDirection::Output;
    output.deferredDispatches = 1;
    events.append(output);
    PrinterProtocol::DuplexTestEvent response;
    response.direction = PrinterProtocol::DuplexTestDirection::Input;
    response.payload = QByteArrayLiteral("ack-after-zlp");
    events.append(response);

    const PrinterProtocol::DuplexTestResult result =
        PrinterProtocol::runDuplexTransportScenarioForTesting(
            events, QByteArrayLiteral("request"));
    QVERIFY2(result.writeSucceeded, qPrintable(result.error));
    QVERIFY(result.responseReceived);
    QCOMPARE(result.response, QByteArrayLiteral("ack-after-zlp"));
    QCOMPARE(result.inputSubmissions, 2);
    QCOMPARE(result.outputSubmissions, 1);
    QCOMPARE(result.maximumConcurrentInputs, 1);
    QCOMPARE(result.inputCompletions, 2);
    QCOMPARE(result.zeroLengthInputCompletions, 1);
    QCOMPARE(result.inputErrors, 0);
    QCOMPARE(result.inputRearmsDuringOutput, 0);
}

void PrinterProtocolTests::
    duplexInputErrorDefersRearmWithoutStarvingOutput() {
    QList<PrinterProtocol::DuplexTestEvent> events;
    PrinterProtocol::DuplexTestEvent inputError;
    inputError.direction = PrinterProtocol::DuplexTestDirection::Input;
    inputError.status = PrinterProtocol::DuplexTestStatus::Error;
    events.append(inputError);
    PrinterProtocol::DuplexTestEvent output;
    output.direction = PrinterProtocol::DuplexTestDirection::Output;
    output.deferredDispatches = 1;
    events.append(output);
    PrinterProtocol::DuplexTestEvent response;
    response.direction = PrinterProtocol::DuplexTestDirection::Input;
    response.payload = QByteArrayLiteral("ack-after-error");
    events.append(response);

    const PrinterProtocol::DuplexTestResult result =
        PrinterProtocol::runDuplexTransportScenarioForTesting(
            events, QByteArrayLiteral("request"));
    QVERIFY2(result.writeSucceeded, qPrintable(result.error));
    QVERIFY(result.responseReceived);
    QCOMPARE(result.response, QByteArrayLiteral("ack-after-error"));
    QCOMPARE(result.inputSubmissions, 2);
    QCOMPARE(result.outputSubmissions, 1);
    QCOMPARE(result.maximumConcurrentInputs, 1);
    QCOMPARE(result.inputCompletions, 2);
    QCOMPARE(result.zeroLengthInputCompletions, 0);
    QCOMPARE(result.inputErrors, 1);
    QCOMPARE(result.inputRearmsDuringOutput, 0);
}

void PrinterProtocolTests::duplexInputRetryBudgetIsBounded() {
    QList<PrinterProtocol::DuplexTestEvent> events;
    PrinterProtocol::DuplexTestEvent output;
    output.direction = PrinterProtocol::DuplexTestDirection::Output;
    events.append(output);
    for (int completion = 0; completion < 10; ++completion) {
        PrinterProtocol::DuplexTestEvent inputError;
        inputError.direction =
            PrinterProtocol::DuplexTestDirection::Input;
        inputError.status =
            PrinterProtocol::DuplexTestStatus::Error;
        events.append(inputError);
    }

    const PrinterProtocol::DuplexTestResult result =
        PrinterProtocol::runDuplexTransportScenarioForTesting(
            events, QByteArrayLiteral("request"), 5000, 100);
    QVERIFY2(result.writeSucceeded, qPrintable(result.error));
    QVERIFY(!result.responseReceived);
    QVERIFY2(result.error.contains(
                 QStringLiteral("persistent"),
                 Qt::CaseInsensitive),
             qPrintable(result.error));
    QCOMPARE(result.outputSubmissions, 1);
    QCOMPARE(result.maximumConcurrentInputs, 1);
    QCOMPARE(result.inputCompletions, 10);
    QCOMPARE(result.zeroLengthInputCompletions, 0);
    QCOMPARE(result.inputErrors, 10);
    QCOMPARE(result.inputRearmsDuringOutput, 0);
    QVERIFY(result.persistentInputFailure);
}

void PrinterProtocolTests::uploadIsResponseDriven() {
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(300000, '\x5a');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    quint64 transferTrackId = 0;
    quint64 nextTrackId = 0;
    std::thread peer([&]() {
        const QList<panorama::wire::v1::Request::BodyCase> expected = {
            panorama::wire::v1::Request::kTransferBegin,
            panorama::wire::v1::Request::kTransferChunk,
            panorama::wire::v1::Request::kTransferChunk,
            panorama::wire::v1::Request::kTransferEnd,
            panorama::wire::v1::Request::kPing
        };
        for (int index = 0; index < expected.size(); ++index) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expected.at(index)) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral("unexpected upload request %1").arg(index);
                }
                return;
            }
            if (!request.has_header() ||
                request.header().track_id() == 0) {
                peerError = QStringLiteral(
                    "upload request does not have a tracked header");
                return;
            }
            const quint32 expectedVersion = index <= 3 ? 0U : 1U;
            if (request.header().version() != expectedVersion) {
                peerError = QStringLiteral(
                    "unexpected request version %1 at index %2")
                                .arg(request.header().version())
                                .arg(index);
                return;
            }
            if (index <= 3) {
                if (transferTrackId == 0) {
                    transferTrackId = request.header().track_id();
                } else if (request.header().track_id() !=
                           transferTrackId) {
                    peerError = QStringLiteral(
                        "FileTransmit changed track ID within one transfer");
                    return;
                }
            } else {
                nextTrackId = request.header().track_id();
                if (nextTrackId == transferTrackId) {
                    peerError = QStringLiteral(
                        "the command after FileTransmit reused its track ID");
                    return;
                }
            }

            auto response = baseResponse(request);
            if (index == 0) {
                if (request.transfer_begin().file_name() !=
                        "test.png.h264_2240x1080" ||
                    request.transfer_begin().file_size() !=
                        static_cast<quint32>(contents.size())) {
                    peerError = QStringLiteral("invalid upload begin fields");
                    return;
                }
                response.mutable_transfer_begin_status()->set_status(
                    panorama::wire::v1::TransferStatus::OK);
            } else if (index < 3) {
                const size_t expectedSize = index == 1
                    ? static_cast<size_t>(0x40000)
                    : static_cast<size_t>(contents.size() - 0x40000);
                if (request.transfer_chunk().file_data().size() != expectedSize) {
                    peerError = QStringLiteral("invalid upload chunk size");
                    return;
                }
                response.mutable_transfer_chunk_status()->set_status(
                    panorama::wire::v1::TransferStatus::OK);
            } else if (index == 3) {
                if (request.transfer_end().file_type() != "media" ||
                    request.transfer_end().checksum() != 0) {
                    peerError = QStringLiteral("invalid upload end fields");
                    return;
                }
                response.mutable_transfer_end_status()->set_status(
                    panorama::wire::v1::TransferStatus::OK);
            } else {
                response.mutable_pong()->set_payload(
                    request.ping().payload());
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString uploadedName;
    QString error;
    qint64 finalProgress = 0;
    const PrinterProtocol::OperationContext context;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY2(protocol.uploadMedia(
                 QStringLiteral("test-endpoint"), media.fileName(),
                 QStringLiteral("test.png.h264_2240x1080"),
                 &uploadedName, &error,
                 [&finalProgress](qint64 sent, qint64) { finalProgress = sent; },
                 context, &mutation),
             qPrintable(error));
    QCOMPARE(uploadedName, QStringLiteral("test.png.h264_2240x1080"));
    QCOMPARE(finalProgress, static_cast<qint64>(contents.size()));
    QCOMPARE(mutation.outcome, PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(mutation.stage, QStringLiteral("Ending"));
    QCOMPARE(mutation.bytesSent, static_cast<qint64>(contents.size()));
    QCOMPARE(mutation.totalBytes, static_cast<qint64>(contents.size()));
    QString pingPayload;
    QVERIFY2(protocol.trackedPingForTesting(
                 QStringLiteral("test-endpoint"), &pingPayload, &error,
                 context),
             qPrintable(error));
    QCOMPARE(pingPayload, QStringLiteral("hello?"));
    peer.join();
    ::close(sockets[1]);
    QVERIFY(transferTrackId != 0);
    QVERIFY(nextTrackId != 0);
    QVERIFY(nextTrackId != transferTrackId);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::uploadDataUsesDedicatedWriteDeadline() {
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(17 * 0x40000, '\x4d');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    const int socketBufferSize = 4096;
    QVERIFY(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                         &socketBufferSize, sizeof(socketBufferSize)) == 0);
    QVERIFY(::setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF,
                         &socketBufferSize, sizeof(socketBufferSize)) == 0);

    QString peerError;
    std::thread peer([&]() {
        constexpr int delayedRequestIndex = 17;
        constexpr int requestCount = 19;
        for (int index = 0; index < requestCount; ++index) {
            if (index == delayedRequestIndex) {
                pollfd requestReady{};
                requestReady.fd = sockets[1];
                requestReady.events = POLLIN;
                if (::poll(&requestReady, 1, kPeerTimeoutMs) != 1 ||
                    (requestReady.revents & POLLIN) == 0) {
                    peerError = QStringLiteral(
                        "delayed upload request did not reach the peer");
                    return;
                }

                const int timerFd = ::timerfd_create(
                    CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
                if (timerFd < 0) {
                    peerError = QStringLiteral("failed to create delay timer");
                    return;
                }
                itimerspec delay{};
                delay.it_value.tv_nsec = 120 * 1000 * 1000;
                if (::timerfd_settime(timerFd, 0, &delay, nullptr) != 0) {
                    peerError = QStringLiteral("failed to arm delay timer");
                    ::close(timerFd);
                    return;
                }
                pollfd timerReady{};
                timerReady.fd = timerFd;
                timerReady.events = POLLIN;
                if (::poll(&timerReady, 1, 500) != 1 ||
                    (timerReady.revents & POLLIN) == 0) {
                    peerError = QStringLiteral("bounded delay timer did not fire");
                    ::close(timerFd);
                    return;
                }
                quint64 expirations = 0;
                if (::read(timerFd, &expirations, sizeof(expirations)) !=
                    static_cast<ssize_t>(sizeof(expirations))) {
                    peerError = QStringLiteral("failed to consume delay timer");
                    ::close(timerFd);
                    return;
                }
                ::close(timerFd);
            }

            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError)) {
                return;
            }
            auto response = baseResponse(request);
            if (index == 0) {
                if (request.body_case() !=
                    panorama::wire::v1::Request::kTransferBegin) {
                    peerError = QStringLiteral("missing upload begin request");
                    return;
                }
                response.mutable_transfer_begin_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            } else if (index == requestCount - 1) {
                if (request.body_case() !=
                    panorama::wire::v1::Request::kTransferEnd) {
                    peerError = QStringLiteral("missing upload end request");
                    return;
                }
                response.mutable_transfer_end_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            } else {
                if (request.body_case() !=
                        panorama::wire::v1::Request::kTransferChunk ||
                    request.transfer_chunk().file_data().size() !=
                        static_cast<size_t>(0x40000)) {
                    peerError = QStringLiteral("invalid upload data request");
                    return;
                }
                response.mutable_transfer_chunk_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }
    });

    PrinterProtocol protocol(60);
    protocol.setFileTransmitDataWriteTimeoutForTesting(500);
    protocol.adoptFileDescriptorForTesting(sockets[0],
                                           QStringLiteral("test-endpoint"));
    QString uploadedName;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    const PrinterProtocol::OperationContext context;
    const bool uploadOk = protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("large.mp4.h264_2240x1080"),
        &uploadedName, &error, {}, context, &mutation);

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY2(uploadOk, qPrintable(error));
    QCOMPARE(mutation.outcome, PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(mutation.bytesSent, static_cast<qint64>(contents.size()));
    QCOMPARE(uploadedName,
             QStringLiteral("large.mp4.h264_2240x1080"));

}

void PrinterProtocolTests::uploadWaitsForDelayedBoundaryAckWithIdleKeepalive() {
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(17 * 0x40000, '\x6b');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));

    QString peerError;
    quint64 transferTrackId = 0;
    std::thread peer([&]() {
        constexpr int delayedRequestIndex = 17;
        constexpr int requestCount = 19;
        for (int index = 0; index < requestCount; ++index) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError,
                             kPeerTimeoutMs * 2)) {
                return;
            }
            if (!request.has_header() ||
                request.header().track_id() == 0) {
                peerError = QStringLiteral(
                    "delayed upload request is not tracked");
                return;
            }
            if (transferTrackId == 0) {
                transferTrackId = request.header().track_id();
            } else if (request.header().track_id() !=
                       transferTrackId) {
                peerError = QStringLiteral(
                    "delayed FileTransmit changed track ID");
                return;
            }

            if (index == delayedRequestIndex) {
                const int timerFd = ::timerfd_create(
                    CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
                if (timerFd < 0) {
                    peerError = QStringLiteral(
                        "failed to create delayed ACK timer");
                    return;
                }
                itimerspec delay{};
                delay.it_value.tv_sec = 2;
                delay.it_value.tv_nsec = 200 * 1000 * 1000;
                if (::timerfd_settime(timerFd, 0, &delay, nullptr) != 0) {
                    peerError = QStringLiteral(
                        "failed to arm delayed ACK timer");
                    ::close(timerFd);
                    return;
                }
                pollfd waits[2]{};
                waits[0].fd = sockets[1];
                waits[0].events = POLLIN;
                waits[1].fd = timerFd;
                waits[1].events = POLLIN;
                const int pollResult = ::poll(waits, 2, 3000);
                if (pollResult < 1 ||
                    (waits[0].revents & POLLIN) == 0) {
                    peerError = QStringLiteral(
                        "FileTransmit did not send idle Ping before delayed DataStatus");
                    ::close(timerFd);
                    return;
                }
                panorama::wire::v1::Request keepalive;
                if (!readRequest(sockets[1], &keepalive, &peerError) ||
                    !keepalive.has_header() ||
                    keepalive.header().ByteSizeLong() != 0 ||
                    keepalive.body_case() !=
                        panorama::wire::v1::Request::kPing ||
                    keepalive.ping().payload() != "hello?") {
                    if (peerError.isEmpty()) {
                        peerError = QStringLiteral(
                            "FileTransmit idle frame was not UDB Ping");
                    }
                    ::close(timerFd);
                    return;
                }
                auto keepaliveResponse = baseResponse(keepalive);
                keepaliveResponse.mutable_pong()->set_payload(
                    keepalive.ping().payload());
                if (!writeResponse(sockets[1], keepaliveResponse,
                                   &peerError)) {
                    ::close(timerFd);
                    return;
                }
                pollfd timerReady{};
                timerReady.fd = timerFd;
                timerReady.events = POLLIN;
                if (::poll(&timerReady, 1, 1000) != 1 ||
                    (timerReady.revents & POLLIN) == 0) {
                    peerError = QStringLiteral(
                        "delayed DataStatus timer did not fire after idle Ping");
                    ::close(timerFd);
                    return;
                }
                quint64 expirations = 0;
                if (::read(timerFd, &expirations,
                           sizeof(expirations)) !=
                    static_cast<ssize_t>(sizeof(expirations))) {
                    peerError = QStringLiteral(
                        "failed to consume delayed ACK timer");
                    ::close(timerFd);
                    return;
                }
                ::close(timerFd);
            }

            auto response = baseResponse(request);
            if (index == 0) {
                if (request.body_case() !=
                    panorama::wire::v1::Request::kTransferBegin) {
                    peerError = QStringLiteral(
                        "missing delayed upload begin");
                    return;
                }
                response.mutable_transfer_begin_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            } else if (index == requestCount - 1) {
                if (request.body_case() !=
                    panorama::wire::v1::Request::kTransferEnd) {
                    peerError = QStringLiteral(
                        "missing delayed upload end");
                    return;
                }
                response.mutable_transfer_end_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            } else {
                if (request.body_case() !=
                        panorama::wire::v1::Request::kTransferChunk ||
                    request.transfer_chunk().file_data().size() !=
                        static_cast<size_t>(0x40000)) {
                    peerError = QStringLiteral(
                        "invalid delayed upload data request");
                    return;
                }
                response.mutable_transfer_chunk_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }
    });

    PrinterProtocol protocol(60);
    protocol.setFileTransmitDataWriteTimeoutForTesting(500);
    protocol.setFileTransmitResponseTimeoutForTesting(3000);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.maintainKeepalive = true;
    QString uploadedName;
    QString error;
    PrinterProtocol::MutationDetails mutation;
    QElapsedTimer uploadTimer;
    uploadTimer.start();
    const bool uploadOk = protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("delayed.mp4.h264_2240x1080"),
        &uploadedName, &error, {}, context, &mutation);

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QVERIFY2(uploadOk, qPrintable(error));
    QVERIFY(uploadTimer.elapsed() >= 2000);
    QVERIFY(uploadTimer.elapsed() < 8000);
    QVERIFY(transferTrackId != 0);
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::Succeeded);
    QCOMPARE(mutation.bytesSent,
             static_cast<qint64>(contents.size()));
    QCOMPARE(uploadedName,
             QStringLiteral("delayed.mp4.h264_2240x1080"));
}

void PrinterProtocolTests::uploadEndTimeoutIsFinalizationUnknown() {
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(4096, '\x71');
    QCOMPARE(media.write(contents),
             static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError),
             qPrintable(socketError));
    QString peerError;
    std::thread peer([&]() {
        const QList<panorama::wire::v1::Request::BodyCase> expected = {
            panorama::wire::v1::Request::kTransferBegin,
            panorama::wire::v1::Request::kTransferChunk,
            panorama::wire::v1::Request::kTransferEnd
        };
        quint64 transferTrackId = 0;
        for (int index = 0; index < expected.size(); ++index) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expected.at(index)) {
                if (peerError.isEmpty()) {
                    peerError = QStringLiteral(
                        "unexpected request before End timeout");
                }
                return;
            }
            if (!request.has_header() ||
                request.header().version() != 0 ||
                request.header().track_id() == 0) {
                peerError = QStringLiteral(
                    "FileTransmit request does not match UDB header");
                return;
            }
            if (transferTrackId == 0) {
                transferTrackId = request.header().track_id();
            } else if (request.header().track_id() != transferTrackId) {
                peerError = QStringLiteral(
                    "FileTransmit changed track before End timeout");
                return;
            }
            if (index == expected.size() - 1) {
                continue;
            }
            auto response = baseResponse(request);
            if (index == 0) {
                response.mutable_transfer_begin_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            } else {
                response.mutable_transfer_chunk_status()
                    ->set_status(
                        panorama::wire::v1::TransferStatus::OK);
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }

        pollfd closeDescriptor{};
        closeDescriptor.fd = sockets[1];
        closeDescriptor.events = POLLIN;
        if (::poll(&closeDescriptor, 1, kPeerTimeoutMs) <= 0) {
            peerError = QStringLiteral(
                "End timeout did not close the transport epoch");
            return;
        }
        char byte = 0;
        if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) != 0) {
            peerError = QStringLiteral(
                "End timeout transport did not close cleanly");
        }
    });

    PrinterProtocol protocol(80);
    protocol.setFileTransmitResponseTimeoutForTesting(120);
    protocol.adoptFileDescriptorForTesting(
        sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    QString uploadedName;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("end-timeout.mp4.h264_2240x1080"),
        &uploadedName, &error, {},
        PrinterProtocol::OperationContext{}, &mutation));
    QVERIFY(error.contains(QStringLiteral("timed out"),
                           Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::FinalizationUnknown);
    QCOMPARE(mutation.stage, QStringLiteral("Ending"));
    QCOMPARE(mutation.bytesSent,
             static_cast<qint64>(contents.size()));
    QCOMPARE(mutation.totalBytes,
             static_cast<qint64>(contents.size()));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::uploadFailureClosesSession_data() {
    QTest::addColumn<int>("failureStage");
    QTest::newRow("begin") << 0;
    QTest::newRow("data") << 1;
    QTest::newRow("end") << 2;
}

void PrinterProtocolTests::uploadFailureClosesSession() {
    QFETCH(int, failureStage);
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(4096, '\x33');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    QString peerError;
    std::thread peer([&]() {
        const QList<panorama::wire::v1::Request::BodyCase> expected = {
            panorama::wire::v1::Request::kTransferBegin,
            panorama::wire::v1::Request::kTransferChunk,
            panorama::wire::v1::Request::kTransferEnd
        };
        for (int stage = 0; stage <= failureStage; ++stage) {
            panorama::wire::v1::Request request;
            if (!readRequest(sockets[1], &request, &peerError) ||
                request.body_case() != expected.at(stage)) {
                peerError = QStringLiteral("unexpected request at failing stage %1").arg(stage);
                return;
            }
            auto response = baseResponse(request);
            const auto status = stage == failureStage
                ? panorama::wire::v1::TransferStatus::FILE_ERROR
                : panorama::wire::v1::TransferStatus::OK;
            if (stage == 0) {
                response.mutable_transfer_begin_status()->set_status(status);
            } else if (stage == 1) {
                response.mutable_transfer_chunk_status()->set_status(status);
            } else {
                response.mutable_transfer_end_status()->set_status(status);
            }
            if (!writeResponse(sockets[1], response, &peerError)) {
                return;
            }
        }

        pollfd closeDescriptor{};
        closeDescriptor.fd = sockets[1];
        closeDescriptor.events = POLLIN;
        if (::poll(&closeDescriptor, 1, kPeerTimeoutMs) <= 0) {
            peerError = QStringLiteral(
                "failed upload did not close its transport epoch");
            return;
        }
        char byte = 0;
        if (::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT) != 0) {
            peerError = QStringLiteral(
                "failed upload transport did not close cleanly");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    QString error;
    QString uploadedName;
    const PrinterProtocol::OperationContext context;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("failure.mp4.h264_2240x1080"),
        &uploadedName, &error, {}, context, &mutation));
    QVERIFY(error.contains(QStringLiteral("file error"), Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome,
             failureStage == 0
                 ? PrinterProtocol::MutationOutcome::Rejected
                 : PrinterProtocol::MutationOutcome::PartialOrUnknown);
    QCOMPARE(mutation.stage,
             failureStage == 0
                 ? QStringLiteral("Beginning")
                 : failureStage == 1
                     ? QStringLiteral("Transferring")
                     : QStringLiteral("Ending"));
    QCOMPARE(mutation.bytesSent,
             failureStage == 2 ? static_cast<qint64>(contents.size()) : 0);
    QCOMPARE(mutation.totalBytes, static_cast<qint64>(contents.size()));

    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::uploadCancellationStopsBeforeNextChunk() {
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(600000, '\x44');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    std::atomic_bool cancelled{false};
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request beginRequest;
        if (!readRequest(sockets[1], &beginRequest, &peerError) ||
            beginRequest.body_case() !=
                panorama::wire::v1::Request::kTransferBegin) {
            peerError = QStringLiteral("missing upload begin before cancellation");
            return;
        }
        auto beginResponse = baseResponse(beginRequest);
        beginResponse.mutable_transfer_begin_status()->set_status(
            panorama::wire::v1::TransferStatus::OK);
        if (!writeResponse(sockets[1], beginResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request dataRequest;
        if (!readRequest(sockets[1], &dataRequest, &peerError) ||
            dataRequest.body_case() !=
                panorama::wire::v1::Request::kTransferChunk) {
            peerError = QStringLiteral("missing first data chunk before cancellation");
            return;
        }
        auto dataResponse = baseResponse(dataRequest);
        dataResponse.mutable_transfer_chunk_status()->set_status(
            panorama::wire::v1::TransferStatus::OK);
        if (!writeResponse(sockets[1], dataResponse, &peerError)) {
            return;
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = QStringLiteral("upload endpoint did not close after cancellation");
            return;
        }
        char byte = 0;
        const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
        if (received > 0) {
            peerError = QStringLiteral("another data chunk was sent after cancellation");
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            peerError = QStringLiteral("failed to inspect cancelled upload endpoint");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    PrinterProtocol::OperationContext context;
    context.isCancelled = [&cancelled]() {
        return cancelled.load(std::memory_order_acquire);
    };
    QString error;
    QString uploadedName;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("cancel.mp4.h264_2240x1080"),
        &uploadedName, &error,
        [&cancelled](qint64 sent, qint64) {
            if (sent > 0) {
                cancelled.store(true, std::memory_order_release);
            }
        },
        context, &mutation));
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    QCOMPARE(mutation.stage, QStringLiteral("Transferring"));
    QCOMPARE(mutation.bytesSent, qint64(0x40000));
    QCOMPARE(mutation.totalBytes, static_cast<qint64>(contents.size()));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

void PrinterProtocolTests::uploadSourceMutationIsRejected_data() {
    QTest::addColumn<bool>("growFile");
    QTest::newRow("grow") << true;
    QTest::newRow("shrink") << false;
}

void PrinterProtocolTests::uploadSourceMutationIsRejected() {
    QFETCH(bool, growFile);
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray contents(300000, '\x55');
    QCOMPARE(media.write(contents), static_cast<qint64>(contents.size()));
    media.flush();

    int sockets[2] = {-1, -1};
    QString socketError;
    QVERIFY2(createSocketPair(sockets, &socketError), qPrintable(socketError));
    QString peerError;
    std::thread peer([&]() {
        panorama::wire::v1::Request beginRequest;
        if (!readRequest(sockets[1], &beginRequest, &peerError)) {
            return;
        }
        auto beginResponse = baseResponse(beginRequest);
        beginResponse.mutable_transfer_begin_status()->set_status(
            panorama::wire::v1::TransferStatus::OK);
        if (!writeResponse(sockets[1], beginResponse, &peerError)) {
            return;
        }

        panorama::wire::v1::Request dataRequest;
        if (!readRequest(sockets[1], &dataRequest, &peerError) ||
            dataRequest.transfer_chunk().file_data().size() != 0x40000) {
            peerError = QStringLiteral("unexpected first chunk before source mutation");
            return;
        }
        auto dataResponse = baseResponse(dataRequest);
        dataResponse.mutable_transfer_chunk_status()->set_status(
            panorama::wire::v1::TransferStatus::OK);
        if (!writeResponse(sockets[1], dataResponse, &peerError)) {
            return;
        }

        pollfd descriptor{};
        descriptor.fd = sockets[1];
        descriptor.events = POLLIN;
        const int pollResult = ::poll(&descriptor, 1, kPeerTimeoutMs);
        if (pollResult <= 0) {
            peerError = QStringLiteral("upload endpoint remained open after source mutation");
            return;
        }
        char byte = 0;
        const ssize_t received = ::recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT);
        if (received > 0) {
            peerError = QStringLiteral("upload sent bytes beyond the stable declared source");
        }
    });

    PrinterProtocol protocol(500);
    protocol.adoptFileDescriptorForTesting(sockets[0], QStringLiteral("test-endpoint"));
    bool mutated = false;
    QString error;
    QString uploadedName;
    const PrinterProtocol::OperationContext context;
    PrinterProtocol::MutationDetails mutation;
    QVERIFY(!protocol.uploadMedia(
        QStringLiteral("test-endpoint"), media.fileName(),
        QStringLiteral("mutation.mp4.h264_2240x1080"),
        &uploadedName, &error,
        [&](qint64 sent, qint64) {
            if (mutated || sent <= 0) {
                return;
            }
            QFile mutation(media.fileName());
            if (growFile) {
                if (mutation.open(QIODevice::WriteOnly | QIODevice::Append)) {
                    mutated = mutation.write(QByteArray(100, '\x66')) == 100 &&
                              mutation.flush();
                }
            } else if (mutation.open(QIODevice::ReadWrite)) {
                mutated = mutation.resize(1024) && mutation.flush();
            }
        },
        context, &mutation));
    QVERIFY(mutated);
    QVERIFY(error.contains(QStringLiteral("changed"), Qt::CaseInsensitive));
    QCOMPARE(mutation.outcome,
             PrinterProtocol::MutationOutcome::PartialOrUnknown);
    QCOMPARE(mutation.stage, QStringLiteral("Transferring"));
    QCOMPARE(mutation.bytesSent, qint64(0x40000));
    QCOMPARE(mutation.totalBytes, static_cast<qint64>(contents.size()));
    peer.join();
    ::close(sockets[1]);
    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
}

int main(int argc, char **argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    QApplication application(argc, argv);
    PrinterProtocolTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "printerprotocol_tests.moc"
