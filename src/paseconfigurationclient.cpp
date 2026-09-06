#include "paseconfigurationclient.h"
#include "paseoverlayconfig.h"
#include "runtimecontract.h"
#include "configuration.pb.h"
#include "overlay.pb.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QSet>

#include "printermediahelpers_p.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include <algorithm>

using namespace tryx::printer_media;
using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;

PaseConfigurationClient::PaseConfigurationClient(PrinterTransactionChannel &channel,
                                                 const PrinterProductProfile &profile,
                                                 int deviceInfoReadyTimeoutMs)
    : channel_(channel), productProfile_(profile),
      deviceInfoReadyTimeoutMs_(
          qBound(1, deviceInfoReadyTimeoutMs, kDeviceInformationReadinessDeadlineMs)) {
    channel_.setKeepaliveFrameFactory(&PaseConfigurationClient::makeKeepaliveFrame);
}

bool PaseConfigurationClient::bootstrapSession(
    const QString &devicePath, const OperationContext &context,
    panorama::wire::v1::Response *deviceInfoResponse,
    panorama::wire::v1::Response *sysConfigResponse, QString *errorMessage) {
    panorama::wire::v1::Request deviceInfoRequest;
    deviceInfoRequest.mutable_header()->set_version(1);
    deviceInfoRequest.mutable_device_information_query()->set_dummy("NA");

    panorama::wire::v1::Request sysConfigRequest;
    sysConfigRequest.mutable_header()->set_version(1);
    sysConfigRequest.mutable_system_configuration_query()->set_dummy("NA");

    panorama::wire::v1::Request deviceAuthRequest;
    deviceAuthRequest.mutable_header()->set_version(1);
    deviceAuthRequest.mutable_device_authentication_query()->set_key(1);

    const auto makeBootstrapFrame =
        [errorMessage](const panorama::wire::v1::Request &request) {
            std::string serializedRequest;
            if (!request.SerializeToString(&serializedRequest) ||
                serializedRequest.size() >
                    static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Failed to serialize bounded TRYX session bootstrap request");
                }
                return QByteArray();
            }
            const QByteArray frame = PrinterFrameCodec::encode(
                QByteArray(serializedRequest.data(),
                           static_cast<qsizetype>(serializedRequest.size())));
            if (frame.isEmpty() && errorMessage) {
                *errorMessage = QObject::tr(
                    "Failed to create the bounded TRYX session bootstrap frame");
            }
            return frame;
        };

    const auto readExactBootstrapResponse = [this, &context, errorMessage](
                                                panorama::wire::v1::Response::BodyCase
                                                    expectedBody,
                                                int timeoutMs,
                                                panorama::wire::v1::Response *result) {
        QElapsedTimer responseTimer;
        responseTimer.start();
        int skippedFrames = 0;
        qsizetype skippedResponseBytes = 0;
        while (responseTimer.elapsed() < timeoutMs &&
               skippedFrames <= kMaxSkippedResponseFrames &&
               skippedResponseBytes <= kMaxSkippedResponseBytes) {
            const int remaining = timeoutMs - static_cast<int>(responseTimer.elapsed());
            QByteArray payload;
            if (!channel_.readFrame(&payload, context, qMax(1, remaining),
                                    errorMessage)) {
                return false;
            }

            panorama::wire::v1::Response response;
            if (!response.ParseFromArray(payload.constData(),
                                         static_cast<int>(payload.size()))) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Failed to parse a TRYX session bootstrap response");
                }
                return false;
            }
            const bool expectedBootstrapHeader = response.has_header() &&
                                                 response.header().version() == 1 &&
                                                 response.header().track_id() == 0 &&
                                                 response.header().payload_crc32() == 0;
            const bool staleTrackedResponse = response.has_header() &&
                                              response.header().version() == 1 &&
                                              response.header().track_id() != 0 &&
                                              response.header().payload_crc32() == 0;
            const bool headerlessAsynchronousResponse =
                !response.has_header() &&
                (response.body_case() == panorama::wire::v1::Response::kPong ||
                 response.body_case() ==
                     panorama::wire::v1::Response::kAsynchronousEvent);
            if (staleTrackedResponse || headerlessAsynchronousResponse) {
                ++skippedFrames;
                skippedResponseBytes += payload.size() + 8;
                continue;
            }
            if (!expectedBootstrapHeader) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX session bootstrap response has an unexpected header");
                }
                return false;
            }
            if (response.has_error() &&
                response.error().code() != panorama::wire::v1::ProtocolError::SUCCESS) {
                if (errorMessage) {
                    const QString why =
                        QString::fromStdString(response.error().why()).trimmed();
                    *errorMessage =
                        why.isEmpty()
                            ? QObject::tr(
                                  "TRYX device rejected the session bootstrap with error %1")
                                  .arg(static_cast<int>(response.error().code()))
                            : why;
                }
                return false;
            }
            if (response.body_case() != expectedBody) {
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr(
                            "TRYX session bootstrap response body %1 does not match expected body %2")
                            .arg(static_cast<int>(response.body_case()))
                            .arg(static_cast<int>(expectedBody));
                }
                return false;
            }
            if (result) {
                *result = std::move(response);
            }
            return true;
        }

        if (errorMessage) {
            *errorMessage =
                (skippedFrames > kMaxSkippedResponseFrames ||
                 skippedResponseBytes > kMaxSkippedResponseBytes)
                    ? QObject::tr(
                          "Too many unrelated TRYX session bootstrap response frames")
                    : QObject::tr(
                          "Timed out waiting for an exact TRYX session bootstrap response");
        }
        return false;
    };

    QElapsedTimer readinessTimer;
    readinessTimer.start();
#ifdef TRYX_PROTOCOL_TESTING
    bootstrapReadinessAttemptOffsetsForTesting_.clear();
#endif
    if (!channel_.ensureOpen(devicePath, errorMessage)) {
        return false;
    }
    if (!channel_.drainKeepaliveResponses(context, errorMessage)) {
        channel_.closeDevice();
        return false;
    }

    const QByteArray deviceInfoFrame = makeBootstrapFrame(deviceInfoRequest);
    if (deviceInfoFrame.isEmpty()) {
        channel_.closeDevice();
        return false;
    }

    int deviceInfoAttempts = 0;
    int retryBackoffMs = kDeviceInformationReadinessInitialBackoffMs;
    QString lastDeviceInfoError;
    bool deviceInfoReady = false;
    while (readinessTimer.elapsed() < deviceInfoReadyTimeoutMs_) {
        if (operationIsCancelled(context)) {
            setCancelledError(errorMessage);
            channel_.closeDevice();
            return false;
        }
        ++deviceInfoAttempts;
#ifdef TRYX_PROTOCOL_TESTING
        bootstrapReadinessAttemptOffsetsForTesting_.append(readinessTimer.elapsed());
#endif

        const int writeBudgetMs =
            std::min({channel_.transactionTimeoutMs(), kUdbBootstrapWriteTimeoutMs,
                      qMax(1, deviceInfoReadyTimeoutMs_ -
                                  static_cast<int>(readinessTimer.elapsed()))});
        qsizetype writtenBytes = 0;
        WriteFailureKind writeFailure = WriteFailureKind::None;
        bool writeSucceeded = false;
#ifdef TRYX_PROTOCOL_TESTING
        if (bootstrapZeroByteWriteFailuresForTesting_ > 0) {
            --bootstrapZeroByteWriteFailuresForTesting_;
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Simulated confirmed zero-byte TRYX DeviceInfo OUT");
            }
            writeFailure = WriteFailureKind::RetryableNoWrite;
        } else {
#endif
            writeSucceeded =
                channel_.writeAll(deviceInfoFrame, context, writeBudgetMs, errorMessage,
                                  &writtenBytes, &writeFailure);
#ifdef TRYX_PROTOCOL_TESTING
        }
#endif

        if (!writeSucceeded) {
            const bool confirmedZeroByteOut =
                writtenBytes == 0 &&
                writeFailure == WriteFailureKind::RetryableNoWrite &&
                !operationIsCancelled(context);
            if (!confirmedZeroByteOut) {
                channel_.closeDevice();
                return false;
            }

            lastDeviceInfoError = errorMessage ? *errorMessage : QString();
            const int remainingMs =
                deviceInfoReadyTimeoutMs_ - static_cast<int>(readinessTimer.elapsed());
            if (remainingMs <= 0) {
                break;
            }
            const int boundedBackoffMs = qMin(retryBackoffMs, remainingMs);
            if (context.onReadinessProbeRetry) {
                ReadinessRetryInfo retryInfo;
                retryInfo.attempt = deviceInfoAttempts;
                retryInfo.expectedBytes = deviceInfoFrame.size();
                retryInfo.actualBytes = writtenBytes;
                retryInfo.elapsedMs = static_cast<int>(readinessTimer.elapsed());
                retryInfo.backoffMs = boundedBackoffMs;
                retryInfo.transferStatus = lastDeviceInfoError;
                context.onReadinessProbeRetry(retryInfo);
            }
            if (!waitForReadinessBackoff(boundedBackoffMs, context, errorMessage)) {
                channel_.closeDevice();
                return false;
            }
            retryBackoffMs =
                qMin(retryBackoffMs * 2, kDeviceInformationReadinessMaximumBackoffMs);
            continue;
        }

        const int responseBudgetMs =
            deviceInfoReadyTimeoutMs_ - static_cast<int>(readinessTimer.elapsed());
        panorama::wire::v1::Response response;
        const bool exactDeviceInfoReceived =
            responseBudgetMs > 0 &&
            readExactBootstrapResponse(panorama::wire::v1::Response::kDeviceInformation,
                                       responseBudgetMs, &response);
        if (!exactDeviceInfoReceived) {
            const QString terminalError = errorMessage ? *errorMessage : QString();
            channel_.closeDevice();
            if (errorMessage) {
                *errorMessage =
                    QObject::tr(
                        "TRYX DeviceInfo readiness failed after %1 attempts: %2")
                        .arg(deviceInfoAttempts)
                        .arg(terminalError);
            }
            return false;
        }
        if (deviceInfoResponse) {
            *deviceInfoResponse = std::move(response);
        }
        deviceInfoReady = true;
        if (context.onDeviceInfoReady) {
            context.onDeviceInfoReady();
        }
        break;
    }

    if (!deviceInfoReady) {
        channel_.closeDevice();
        if (errorMessage) {
            *errorMessage =
                QObject::tr(
                    "TRYX DeviceInfo readiness did not become ready after %1 attempts within %2 ms: %3")
                    .arg(deviceInfoAttempts)
                    .arg(deviceInfoReadyTimeoutMs_)
                    .arg(lastDeviceInfoError);
        }
        return false;
    }

    const auto executeBootstrapExchangeOnce =
        [this, &context, errorMessage, &makeBootstrapFrame,
         &readExactBootstrapResponse](
            const panorama::wire::v1::Request &request,
            panorama::wire::v1::Response::BodyCase expectedBody,
            panorama::wire::v1::Response *result) {
            if (operationIsCancelled(context)) {
                setCancelledError(errorMessage);
                return false;
            }
            if (!channel_.drainKeepaliveResponses(context, errorMessage)) {
                return false;
            }
            const QByteArray frame = makeBootstrapFrame(request);
            if (frame.isEmpty()) {
                return false;
            }
            qsizetype writtenBytes = 0;
            WriteFailureKind writeFailure = WriteFailureKind::None;
            if (!channel_.writeAll(
                    frame, context,
                    qMin(channel_.transactionTimeoutMs(), kUdbBootstrapWriteTimeoutMs),
                    errorMessage, &writtenBytes, &writeFailure)) {
                return false;
            }
            return readExactBootstrapResponse(expectedBody,
                                              channel_.transactionTimeoutMs(), result);
        };

    if (!executeBootstrapExchangeOnce(
            sysConfigRequest, panorama::wire::v1::Response::kSystemConfiguration,
            sysConfigResponse) ||
        !executeBootstrapExchangeOnce(
            deviceAuthRequest, panorama::wire::v1::Response::kDeviceAuthentication,
            nullptr)) {
        channel_.closeDevice();
        return false;
    }
    return true;
}

bool PaseConfigurationClient::executeUserConfigurationQueryWithRetry(
    panorama::wire::v1::Request *request, panorama::wire::v1::Response *response,
    const QString &devicePath, const OperationContext &context, QString *errorMessage,
    const QString &queryName) {
    if (!request ||
        request->body_case() != panorama::wire::v1::Request::kUserConfigurationQuery) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Only TRYX user configuration reads may use the bounded query retry");
        }
        return false;
    }
    constexpr auto expectedBody = panorama::wire::v1::Response::kUserConfiguration;
    for (int attempt = 1; attempt <= kIdempotentQueryMaximumAttempts; ++attempt) {
        const bool retryAvailable = attempt < kIdempotentQueryMaximumAttempts;
        TransactionOutcome outcome = TransactionOutcome::NotSent;
        QString attemptError;
        if (response) {
            response->Clear();
        }
        if (channel_.execute(request, expectedBody, response, devicePath, context,
                             &attemptError, &outcome, TransactionProfile::Default,
                             retryAvailable)) {
            if (errorMessage) {
                errorMessage->clear();
            }
            return true;
        }

        if (retryAvailable && outcome == TransactionOutcome::AcknowledgementTimeout &&
            operationIsCancelled(context)) {
            channel_.closeDevice();
            setCancelledError(errorMessage);
            return false;
        }
        if (!retryAvailable || outcome != TransactionOutcome::AcknowledgementTimeout) {
            if (errorMessage) {
                *errorMessage = attemptError;
            }
            return false;
        }

        qWarning().noquote()
            << QStringLiteral(
                   "TRYX idempotent query matching-response timeout; retrying: query=%1 attempt=%2 max_attempts=%3 expected_body=%4 backoff_ms=%5 error=%6")
                   .arg(queryName)
                   .arg(attempt)
                   .arg(kIdempotentQueryMaximumAttempts)
                   .arg(static_cast<int>(expectedBody))
                   .arg(kIdempotentQueryRetryBackoffMs)
                   .arg(attemptError);

        QString backoffError;
        if (!waitForReadinessBackoff(kIdempotentQueryRetryBackoffMs, context,
                                     &backoffError)) {
            channel_.closeDevice();
            if (errorMessage) {
                *errorMessage = backoffError;
            }
            return false;
        }
    }

    if (errorMessage) {
        *errorMessage = QObject::tr("TRYX idempotent query retry budget was exhausted");
    }
    channel_.closeDevice();
    return false;
}

#ifdef TRYX_PROTOCOL_TESTING
void PaseConfigurationClient::setBootstrapZeroByteWriteFailuresForTesting(
    int failureCount) {
    bootstrapZeroByteWriteFailuresForTesting_ = qMax(0, failureCount);
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
QList<qint64>
PaseConfigurationClient::bootstrapReadinessAttemptOffsetsForTesting() const {
    return bootstrapReadinessAttemptOffsetsForTesting_;
}
#endif

QByteArray PaseConfigurationClient::makeKeepaliveFrame(QString *errorMessage) {
    panorama::wire::v1::Request request;
    request.mutable_header();
    request.mutable_ping()->set_payload("hello?");

    std::string serializedRequest;
    if (!request.SerializeToString(&serializedRequest) ||
        serializedRequest.size() >
            static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to serialize bounded TRYX keepalive request");
        }
        return {};
    }

    const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
        serializedRequest.data(), static_cast<qsizetype>(serializedRequest.size())));
    if (frame.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to create the bounded TRYX keepalive frame");
        }
        return {};
    }
    return frame;
}

namespace {

PrinterProtocol::DeviceInfo makeLocalPrinterDeviceInfo(const QString &devicePath) {
    PrinterProtocol::DeviceInfo info;
    info.devicePath = devicePath;
    const PrinterProtocol::DiscoverySnapshot snapshot = PrinterProtocol::discover();
    for (const PrinterProtocol::UsbPrinterDevice &device : snapshot.devices) {
        if (device.devicePath == devicePath) {
            info.manufacturer = device.manufacturer;
            info.usbProduct = device.product;
            info.usbSerial = device.serial;
            break;
        }
    }
    info.productName = info.usbProduct;
    info.serialNumber = info.usbSerial;
    return info;
}

PrinterProtocol::DeviceInfo
makePrinterDeviceInfo(const QString &devicePath,
                      const panorama::wire::v1::DeviceInformation &deviceInfo) {
    PrinterProtocol::DeviceInfo info = makeLocalPrinterDeviceInfo(devicePath);
    info.osName = QString::fromStdString(deviceInfo.os_name());
    info.osVersion = QString::fromStdString(deviceInfo.os_version());
    info.firmwareVersion = QString::fromStdString(deviceInfo.firmware_version());
    info.productName = QString::fromStdString(deviceInfo.product_name());
    info.appVersion = QString::fromStdString(deviceInfo.app_version());
    info.serialNumber = QString::fromStdString(deviceInfo.serial_number());
    info.serialNumberLocked = deviceInfo.serial_number_locked();
    info.chipId = QString::fromStdString(deviceInfo.chip_id());
    return info;
}

bool normalizedReportedProduct(const std::string &rawProduct,
                               QString *reportedProduct) {
    if (!reportedProduct || rawProduct.empty()) {
        return false;
    }
    const QString decoded =
        QString::fromUtf8(rawProduct.data(), static_cast<qsizetype>(rawProduct.size()));
    if (decoded.toUtf8() !=
        QByteArray(rawProduct.data(), static_cast<qsizetype>(rawProduct.size()))) {
        return false;
    }
    for (const uint codePoint : decoded.toUcs4()) {
        const QChar::Category category = QChar::category(codePoint);
        const bool forbiddenCategory =
            category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Separator_Line || category == QChar::Separator_Paragraph;
        const bool forbiddenCodePoint =
            codePoint <= 0x1fU || (codePoint >= 0x7fU && codePoint <= 0x9fU) ||
            codePoint == 0x061cU || codePoint == 0x200eU || codePoint == 0x200fU ||
            (codePoint >= 0x2028U && codePoint <= 0x202eU) ||
            (codePoint >= 0x2066U && codePoint <= 0x206fU);
        if (forbiddenCategory || forbiddenCodePoint) {
            return false;
        }
    }
    const QString normalized = decoded.trimmed();
    if (normalized.isEmpty() || normalized.size() > 128) {
        return false;
    }
    *reportedProduct = normalized;
    return true;
}

PrinterProtocol::DeviceSpecifications
makeDeviceSpecifications(const panorama::wire::v1::SystemConfiguration &configuration) {
    PrinterProtocol::DeviceSpecifications specifications;
    if (!configuration.has_reported_product() || !configuration.has_board_summary() ||
        !configuration.board_summary().has_display_panel() ||
        !configuration.board_summary().display_panel().has_kind() ||
        !configuration.has_video_output() ||
        !configuration.video_output().has_width() ||
        !configuration.video_output().has_height() ||
        !configuration.has_runtime_behavior() ||
        !configuration.runtime_behavior().has_usb_auto_keepalive() ||
        !normalizedReportedProduct(configuration.reported_product(),
                                   &specifications.reportedProductName)) {
        return {};
    }

    const quint32 width = configuration.video_output().width();
    const quint32 height = configuration.video_output().height();
    if (width == 0 || width > 16384 || height == 0 || height > 16384) {
        return {};
    }

    switch (configuration.board_summary().display_panel().kind()) {
    case panorama::wire::v1::DeviceDisplayPanelSummary::DISPLAY_PANEL_LCD:
        specifications.screenType = QStringLiteral("LCD");
        break;
    case panorama::wire::v1::DeviceDisplayPanelSummary::DISPLAY_PANEL_OLED:
        specifications.screenType = QStringLiteral("OLED");
        break;
    default:
        return {};
    }

    specifications.videoOutputWidth = width;
    specifications.videoOutputHeight = height;
    specifications.usbAutoKeepalive =
        configuration.runtime_behavior().usb_auto_keepalive();
    specifications.valid = true;
    return specifications;
}

struct PaseMetricDefinition {
    const char *name;
    const char *title;
    const char *unit;
    quint32 groupId;
    quint32 titleId;
    quint32 valueId;
    quint32 unitId;
    bool dateTime;
};

constexpr std::array<PaseMetricDefinition, 11> kPaseMetricDefinitions{{
    {"CPU Temperature", "CPU TEMP", "°C", 100, 101, 102, 103, false},
    {"CPU Frequency", "CPU Frequency", "MHZ", 101, 104, 105, 106, false},
    {"CPU Usage", "CPU Usage", "%", 102, 107, 108, 109, false},
    {"CPU Power", "CPU Power", "W", 103, 110, 111, 112, false},
    {"GPU Temperature", "GPU TEMP", "°C", 104, 113, 114, 115, false},
    {"GPU Frequency", "GPU Frequency", "MHZ", 105, 116, 117, 118, false},
    {"GPU Usage", "GPU Usage", "%", 106, 119, 120, 121, false},
    {"GPU Power", "GPU Power", "W", 107, 122, 123, 124, false},
    {"Memory Frequency", "Memory Frequency", "MHZ", 108, 125, 126, 127, false},
    {"Memory Usage", "Memory Usage", "%", 109, 128, 129, 130, false},
    {"Date&Time", "", "", 110, 131, 132, 0, true},
}};

const PaseMetricDefinition *paseMetricDefinition(const QString &name) {
    for (const PaseMetricDefinition &definition : kPaseMetricDefinitions) {
        if (name == QString::fromLatin1(definition.name)) {
            return &definition;
        }
    }
    return nullptr;
}

bool paseOverlayMetricSelectionIsValid(
    const PrinterProtocol::PaseOverlayConfig &overlay, QString *errorMessage) {
    const auto areaIsValid = [](const QStringList &metrics) {
        if (metrics.size() > 3) {
            return false;
        }
        QSet<QString> seen;
        for (const QString &metric : metrics) {
            if (metric.isEmpty() || seen.contains(metric) ||
                !paseMetricDefinition(metric)) {
                return false;
            }
            seen.insert(metric);
        }
        return true;
    };
    if (areaIsValid(overlay.left.metrics) && areaIsValid(overlay.right.metrics)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QObject::tr(
            "TRYX metric selections must contain at most three unique canonical metrics per area");
    }
    return false;
}

QList<const PaseMetricDefinition *>
paseSelectedMetrics(const PrinterProtocol::PaseOverlayAreaConfig &area) {
    QList<const PaseMetricDefinition *> selected;
    QSet<QString> seen;
    for (const QString &name : area.metrics) {
        const PaseMetricDefinition *definition = paseMetricDefinition(name);
        if (!definition || seen.contains(name)) {
            continue;
        }
        seen.insert(name);
        selected.append(definition);
    }
    return selected;
}

panorama::wire::v1::OverlayGroup::TextAlignment
paseTextAlign(const QString &alignment) {
    if (alignment.compare(QStringLiteral("Center"), Qt::CaseInsensitive) == 0) {
        return panorama::wire::v1::OverlayGroup::ALIGN_CENTER;
    }
    if (alignment.compare(QStringLiteral("Right"), Qt::CaseInsensitive) == 0) {
        return panorama::wire::v1::OverlayGroup::ALIGN_RIGHT;
    }
    return panorama::wire::v1::OverlayGroup::ALIGN_LEFT;
}

void configurePaseLabel(panorama::wire::v1::OverlayLabel *label, quint32 id,
                        quint32 line, qint32 gapLeft, quint32 size, quint32 color,
                        const QString &text) {
    label->set_label_id(id);
    label->set_line(line);
    label->set_gap_left(gapLeft);
    label->set_text_font("roboto-regular");
    label->set_text_size(size);
    label->set_text_color(color);
    label->set_text(text.toStdString());
}

struct PaseBadgeColors {
    quint32 background = 0;
    quint32 gradient = 0;
};

PaseBadgeColors paseBadgeColors(const QString &text) {
    const QString normalized = text.toLower();
    if (normalized.contains(QStringLiteral("nvidia"))) {
        return {0x00629A00U, 0x0079AB51U};
    }
    if (normalized.contains(QStringLiteral("intel"))) {
        return {0x000068B5U, 0x00566D98U};
    }
    if (normalized.contains(QStringLiteral("amd")) ||
        normalized.contains(QStringLiteral("radeon")) ||
        normalized.contains(QStringLiteral("ryzen"))) {
        return {0x00A92F2CU, 0x00CB6236U};
    }
    return {0x004A4A4AU, 0x00707070U};
}

void configurePaseBadge(panorama::wire::v1::OverlayLabel *label, quint32 id,
                        qint32 gapLeft, const QString &text, bool custom) {
    const PaseBadgeColors colors = custom ? PaseBadgeColors{0x004A4A4AU, 0x00707070U}
                                          : paseBadgeColors(text);
    label->set_label_id(id);
    label->set_gap_left(gapLeft);
    label->set_background(
        panorama::wire::v1::OverlayLabel::BACKGROUND_GRADIENT_HORIZONTAL);
    label->set_background_color(colors.background);
    label->set_gradient_color(colors.gradient);
    label->set_text_font("roboto-regular");
    label->set_text_size(30);
    label->set_text_color(0x00DCDCDCU);
    label->set_text(QStringLiteral("  %1  ").arg(text).toStdString());
}

bool paseAreaHasContent(const PrinterProtocol::PaseOverlayAreaConfig &area) {
    return !area.metrics.isEmpty() || !area.badges.isEmpty();
}

void appendPaseOverlayArea(panorama::wire::v1::OverlayLayout *runConfig,
                           const PrinterProtocol::PaseOverlayConfig &overlay,
                           const PrinterProtocol::PaseOverlayAreaConfig &area,
                           bool rightArea) {
    if (!runConfig || !paseAreaHasContent(area)) {
        return;
    }
    constexpr int kScreenWidth = 2240;
    constexpr int kScreenHeight = 1080;
    constexpr int kTextOffsetX = 60;
    constexpr int kTextOffsetY = -20;
    constexpr int kValueTextSize = 160;
    constexpr int kTagOffsetY = 70;
    const int areaCount = overlay.dualMode ? 2 : 1;
    int areaX = rightArea ? kScreenWidth / areaCount + kTextOffsetX : kTextOffsetX;
    const int groupIdOffset = rightArea ? 100 : 0;
    const auto alignment = paseTextAlign(area.alignment);
    const qint32 titleGap =
        alignment == panorama::wire::v1::OverlayGroup::ALIGN_LEFT ? 13 : 0;
    const QList<const PaseMetricDefinition *> selected = paseSelectedMetrics(area);
    const int metricCount = selected.size();
    const QDateTime now = QDateTime::currentDateTime();

    for (int index = 0; index < metricCount; ++index) {
        const PaseMetricDefinition &definition = *selected.at(index);
        int groupY =
            metricCount == 1
                ? kScreenHeight / 2
                : (kScreenHeight / (metricCount + 1)) * (index + 1) + index * 10;
        groupY -= kValueTextSize / 2;
        int groupWidth = kScreenWidth / areaCount - kTextOffsetX * 2;
        int groupX = areaX;
        if (overlay.waterfallMode) {
            groupWidth = kScreenWidth / 2 - kTextOffsetX * 2 - 50;
            if (overlay.dualMode) {
                if (rightArea) {
                    groupX = kTextOffsetX;
                } else {
                    groupY += kScreenWidth / 2;
                }
            } else if (area.verticalPlacement.compare(QStringLiteral("Bottom"),
                                                      Qt::CaseInsensitive) == 0) {
                groupY += kScreenWidth / 2;
            }
        }

        auto *group = runConfig->add_label_groups();
        group->set_group_id(definition.groupId + groupIdOffset);
        group->set_group_x(static_cast<quint32>(groupX));
        group->set_group_y(static_cast<quint32>(groupY + kTextOffsetY));
        group->set_group_width(static_cast<quint32>(groupWidth));
        group->set_group_height(160);
        group->set_text_align(alignment);
        group->set_line_gap(-10);

        const quint32 titleId = definition.titleId + groupIdOffset;
        const quint32 valueId = definition.valueId + groupIdOffset;
        const quint32 unitId =
            definition.unitId == 0 ? 0 : definition.unitId + groupIdOffset;
        if (definition.dateTime) {
            configurePaseLabel(group->add_labels(), titleId, 1, titleGap, 30,
                               area.textColor,
                               QLocale().toString(now.date(), QLocale::ShortFormat));
            configurePaseLabel(
                group->add_labels(), valueId, 0, 0, 160, area.textColor,
                tryxFormatLocalTime(now.time(), overlay.timeFormat, QLocale()));
            continue;
        }

        configurePaseLabel(group->add_labels(), titleId, 1, titleGap, 30,
                           area.textColor, QString::fromUtf8(definition.title));
        const int initialIndex =
            area.initialLabels.indexOf(QString::fromLatin1(definition.name));
        const QString initialValue =
            initialIndex >= 0 && initialIndex < area.initialValues.size() &&
                    !area.initialValues.at(initialIndex).isEmpty()
                ? area.initialValues.at(initialIndex)
                : QStringLiteral("--");
        const bool temperatureMetric =
            definition.groupId == 100 || definition.groupId == 104;
        const QString defaultUnit =
            temperatureMetric ? tryxTemperatureUnitSymbol(overlay.temperatureUnit)
                              : QString::fromUtf8(definition.unit);
        const QString initialUnit =
            initialIndex >= 0 && initialIndex < area.initialUnits.size() &&
                    !area.initialUnits.at(initialIndex).isEmpty()
                ? area.initialUnits.at(initialIndex)
                : defaultUnit;
        configurePaseLabel(group->add_labels(), valueId, 0, 0, 160, area.textColor,
                           initialValue);
        configurePaseLabel(group->add_labels(), unitId, 0, 0, 36, area.textColor,
                           initialUnit);
    }

    if (area.badges.isEmpty()) {
        return;
    }
    int badgeY = kTagOffsetY;
    int badgeWidth = kScreenWidth / areaCount - kTextOffsetX * 2;
    int badgeX = areaX + 10;
    if (overlay.waterfallMode) {
        badgeWidth = kScreenWidth / 2 - kTextOffsetX * 2 - 30;
        if (overlay.dualMode) {
            if (rightArea) {
                badgeX = kTextOffsetX + 10;
            } else {
                badgeY += kScreenWidth / 2;
            }
        } else if (area.verticalPlacement.compare(QStringLiteral("Bottom"),
                                                  Qt::CaseInsensitive) == 0) {
            badgeY += kScreenWidth / 2;
        }
    }
    auto *badgeGroup = runConfig->add_label_groups();
    badgeGroup->set_group_id(rightArea ? 400 : 300);
    badgeGroup->set_group_x(static_cast<quint32>(badgeX));
    badgeGroup->set_group_y(static_cast<quint32>(badgeY));
    badgeGroup->set_group_width(static_cast<quint32>(badgeWidth));
    badgeGroup->set_text_align(alignment);
    badgeGroup->set_line_gap(1);
    int badgeIndex = 0;
    for (const QString &badge : area.badges) {
        const bool cpu =
            badge.compare(QStringLiteral("CPU Badge"), Qt::CaseInsensitive) == 0 ||
            badge.compare(QStringLiteral("cpu"), Qt::CaseInsensitive) == 0;
        const bool gpu =
            badge.compare(QStringLiteral("GPU Badge"), Qt::CaseInsensitive) == 0 ||
            badge.compare(QStringLiteral("gpu"), Qt::CaseInsensitive) == 0;
        if (!cpu && !gpu) {
            continue;
        }
        const auto &choice = rightArea
            ? (cpu ? overlay.badgeChoices.secondaryCpu : overlay.badgeChoices.secondaryGpu)
            : (cpu ? overlay.badgeChoices.primaryCpu : overlay.badgeChoices.primaryGpu);
        const bool custom = choice.mode == QStringLiteral("Custom");
        const QString text = custom ? choice.text :
            cpu ? (overlay.cpuBadgeText.isEmpty() ? QStringLiteral("CPU")
                                                  : overlay.cpuBadgeText)
                : (overlay.gpuBadgeText.isEmpty() ? QStringLiteral("GPU")
                                                  : overlay.gpuBadgeText);
        configurePaseBadge(
            badgeGroup->add_labels(),
            static_cast<quint32>((rightArea ? 400 : 300) + (cpu ? 1 : 2)),
            badgeIndex > 0 ? 10 : 0, text, custom);
        ++badgeIndex;
    }
    if (badgeGroup->labels().empty()) {
        runConfig->mutable_label_groups()->RemoveLast();
    }
}

panorama::wire::v1::OverlayLayout
buildPaseRunConfig(const PrinterProtocol::PaseOverlayConfig &overlay) {
    panorama::wire::v1::OverlayLayout runConfig;
    appendPaseOverlayArea(&runConfig, overlay, overlay.left, false);
    if (overlay.dualMode) {
        appendPaseOverlayArea(&runConfig, overlay, overlay.right, true);
    }
    return runConfig;
}

void addPaseLabelUpdate(panorama::wire::v1::MetricBatch *batch, quint32 groupId,
                        quint32 labelId, const QString &text) {
    auto *groupUpdate = batch->add_label_groups();
    groupUpdate->set_group_id(groupId);
    auto *labelUpdate = groupUpdate->add_label_texts();
    labelUpdate->set_label_id(labelId);
    labelUpdate->set_text(text.toStdString());
}

} // namespace

PrinterProtocol::Result
PaseConfigurationClient::startDisplaySession(const QString &devicePath,
                                             const OperationContext &context) {
    QString error;
    if (!channel_.openSessionTransport(devicePath, context, &error)) {
        return {false, error, {}, {}};
    }

    panorama::wire::v1::Response bootstrapResponse;
    panorama::wire::v1::Response sysConfigResponse;
    if (!bootstrapSession(devicePath, context, &bootstrapResponse, &sysConfigResponse,
                          &error)) {
        return {false, error, {}, {}};
    }
    if (productProfile_.idleMode == PrinterIdleMode::OverlayLayout &&
        !sendRunConfigTrigger(devicePath, &error, context, nullptr)) {
        channel_.closeDevice();
        return {false, error, {}, {}};
    }
    const DeviceInfo deviceInfo =
        makePrinterDeviceInfo(devicePath, bootstrapResponse.device_information());
    const DeviceSpecifications deviceSpecifications =
        makeDeviceSpecifications(sysConfigResponse.system_configuration());
    channel_.closeDisplayActivationCycle();
    return {true, {}, deviceInfo, deviceSpecifications};
}

PrinterProtocol::Result
PaseConfigurationClient::readDeviceInfo(const QString &devicePath,
                                        const OperationContext &context) {

    panorama::wire::v1::Request request;
    request.mutable_device_information_query();
    panorama::wire::v1::Response response;
    QString error;
    if (!channel_.execute(&request, panorama::wire::v1::Response::kDeviceInformation,
                          &response, devicePath, context, &error)) {
        return {false, error, {}, {}};
    }

    return {
        true, {}, makePrinterDeviceInfo(devicePath, response.device_information()), {}};
}

bool PaseConfigurationClient::applyPresetMedia(const QString &devicePath,
                                               const QString &mediaFile, int brightness,
                                               QString *errorMessage,
                                               const OperationContext &context,
                                               MutationDetails *mutationDetails) {
    return applyPresetMediaWithOverlay(devicePath, mediaFile, brightness,
                                       PaseOverlayConfig{}, errorMessage, context,
                                       mutationDetails);
}

bool PaseConfigurationClient::applyPresetMediaWithOverlay(
    const QString &devicePath, const QString &mediaFile, int brightness,
    const PaseOverlayConfig &overlay, QString *errorMessage,
    const OperationContext &context, MutationDetails *mutationDetails) {
    PaseApplyConfig config;
    config.media = {mediaFile};
    config.screenMode = QStringLiteral("Full Screen");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    config.replaceOverlay = true;
    config.overlay = overlay;
    if (brightness >= 0) {
        config.display.brightnessPresent = true;
        config.display.brightness = brightness;
    }
    return applyPaseConfiguration(devicePath, config, errorMessage, context,
                                  mutationDetails);
}

PrinterProtocol::PaseDisplayStateResult
PaseConfigurationClient::readPaseDisplayState(const QString &devicePath,
                                              const OperationContext &context) {
    PaseDisplayStateResult result;
    if (!productProfile_.displayConfigurationSupported) {
        result.error = unsupportedCapabilityError(
            productProfile_, QStringLiteral("display configuration"));
        return result;
    }
    panorama::wire::v1::Request request;
    request.mutable_user_configuration_query();
    panorama::wire::v1::Response response;
    if (!executeUserConfigurationQueryWithRetry(
            &request, &response, devicePath, context, &result.error,
            QStringLiteral("user-configuration-state"))) {
        return result;
    }

    const panorama::wire::v1::UserConfiguration &config = response.user_configuration();
    if (!config.has_display_config() || !config.has_work_config()) {
        result.error = QObject::tr(
            "TRYX user configuration is missing display or work configuration");
        return result;
    }
    const auto &display = config.display_config();
    const auto &work = config.work_config();
    result.state.backlightEnabled = display.backlight_enable();
    result.state.brightness =
        static_cast<int>(qMin<quint32>(display.backlight_brightness(), 100U));
    result.state.mirrorMode = display.media_rotation() == 180U;
    result.state.waterfallMode = display.ui_rotation() == 90U;
    if (config.has_standby_config()) {
        result.state.standbyEnabled = config.standby_config().enable();
        result.state.standbyMedia =
            QString::fromStdString(config.standby_config().media_file());
    }
    if (!decodePaseActiveLayout(work, &result.state.screenMode, &result.state.playMode,
                                &result.state.media, &result.error)) {
        return result;
    }
    result.success = true;
    return result;
}

bool PaseConfigurationClient::applyPaseConfiguration(const QString &devicePath,
                                                     const PaseApplyConfig &config,
                                                     QString *errorMessage,
                                                     const OperationContext &context,
                                                     MutationDetails *mutationDetails,
                                                     PaseDisplayState *appliedState) {
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("Validating");
    }
    const auto markRejected = [mutationDetails]() {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
    };
    if (!productProfile_.displayConfigurationSupported) {
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("display configuration"));
        }
        markRejected();
        return false;
    }
    if (!tryx::pase_overlay_config::paseBadgeChoicesAreValid(config.overlay, productProfile_.productId, errorMessage)) {
        markRejected();
        return false;
    }
    if (config.replaceOverlay && !productProfile_.overlayMetricsSupported) {
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("overlay metrics"));
        }
        markRejected();
        return false;
    }
    if (!config.mediaPresent && !config.display.brightnessPresent &&
        !config.display.standbyPresent && !config.display.backlightPresent &&
        !config.display.orientationPresent && !config.replaceOverlay) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("The PASE configuration request does not contain a change");
        }
        markRejected();
        return false;
    }
    if (config.display.standbyPresent) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "PASE standby configuration is fixed by the firmware; use display power control instead");
        }
        markRejected();
        return false;
    }
    if (config.mediaPresent) {
        const bool fullScreen = config.screenMode == QStringLiteral("Full Screen");
        const bool splitScreen =
            config.screenMode == QStringLiteral("Screen Splitting");
        if (!fullScreen && !splitScreen) {
            if (errorMessage) {
                *errorMessage = QObject::tr("The PASE screen mode is not supported");
            }
            markRejected();
            return false;
        }
        if ((splitScreen && config.playMode != QStringLiteral("Single")) ||
            (fullScreen && config.playMode != QStringLiteral("Single") &&
             config.playMode != QStringLiteral("Loop") &&
             config.playMode != QStringLiteral("Shuffle"))) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The PASE play mode is not supported for this screen mode");
            }
            markRejected();
            return false;
        }
        const int expectedMediaCount = splitScreen ? 2 : 1;
        if (config.media.size() != expectedMediaCount) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("The PASE screen mode requires %1 media file(s)")
                        .arg(expectedMediaCount);
            }
            markRejected();
            return false;
        }
        for (const QString &mediaFile : config.media) {
            if (!isSafeDeviceMediaName(mediaFile)) {
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr("Printer-class media name is not supported: %1")
                            .arg(mediaFile);
                }
                markRejected();
                return false;
            }
        }
    }
    if (config.display.brightnessPresent &&
        (config.display.brightness < 0 || config.display.brightness > 100)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("PASE brightness must be between 0 and 100");
        }
        markRejected();
        return false;
    }

    panorama::wire::v1::Request getRequest;
    getRequest.mutable_user_configuration_query();
    panorama::wire::v1::Response getResponse;
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("ReadingConfig");
    }
    if (!executeUserConfigurationQueryWithRetry(
            &getRequest, &getResponse, devicePath, context, errorMessage,
            QStringLiteral("user-configuration-preflight"))) {
        if (mutationDetails && operationIsCancelled(context)) {
            mutationDetails->outcome = MutationOutcome::Cancelled;
        }
        return false;
    }

    panorama::wire::v1::UserConfiguration userConfig = getResponse.user_configuration();
    if (config.mediaPresent && !userConfig.has_work_config()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "TRYX user configuration has no work configuration; refusing a synthetic write");
        }
        markRejected();
        return false;
    }
    if ((config.display.brightnessPresent || config.display.backlightPresent ||
         config.display.orientationPresent) &&
        !userConfig.has_display_config()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "TRYX user configuration has no display configuration; refusing a synthetic write");
        }
        markRejected();
        return false;
    }

    if (config.mediaPresent) {
        auto *workConfig = userConfig.mutable_work_config();
        if (config.screenMode == QStringLiteral("Screen Splitting")) {
            workConfig->set_media_mode(
                panorama::wire::v1::WorkConfiguration::MEDIA_DUAL);
            workConfig->set_loop_mode(
                panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
            workConfig->set_dual_mode_left_media_file(config.media.at(0).toStdString());
            workConfig->set_dual_mode_right_media_file(
                config.media.at(1).toStdString());
        } else {
            workConfig->set_media_mode(
                panorama::wire::v1::WorkConfiguration::MEDIA_SINGLE);
            if (config.playMode == QStringLiteral("Loop")) {
                workConfig->set_loop_mode(
                    panorama::wire::v1::WorkConfiguration::LOOP_ALL);
            } else if (config.playMode == QStringLiteral("Shuffle")) {
                workConfig->set_loop_mode(
                    panorama::wire::v1::WorkConfiguration::LOOP_RANDOM);
            } else {
                workConfig->set_loop_mode(
                    panorama::wire::v1::WorkConfiguration::LOOP_SINGLE);
            }
            workConfig->set_single_mode_media_file(
                config.media.constFirst().toStdString());
        }
    }
    if (config.display.brightnessPresent) {
        userConfig.mutable_display_config()->set_backlight_brightness(
            static_cast<quint32>(config.display.brightness));
    }
    if (config.display.backlightPresent) {
        userConfig.mutable_display_config()->set_backlight_enable(
            config.display.backlightEnabled);
    }
    if (config.display.orientationPresent) {
        auto *display = userConfig.mutable_display_config();
        display->set_mirror(false);
        display->set_ui_rotation(config.display.waterfallMode ? 90U : 0U);
        display->set_media_rotation(config.display.mirrorMode ? 180U : 0U);
    }

    if (!sendUserConfigWithOutcome(devicePath, userConfig, errorMessage, context,
                                   mutationDetails)) {
        return false;
    }
    bool activationRejected = false;
    if (!activateAcceptedConfig(devicePath, errorMessage, context, mutationDetails,
                                &config.overlay, &activationRejected) &&
        !activationRejected) {
        return false;
    }

    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("VerifyingConfig");
    }
    const PaseDisplayStateResult readback = readPaseDisplayState(devicePath, context);
    if (!readback.success) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
        }
        if (errorMessage) {
            *errorMessage =
                readback.error.isEmpty()
                    ? QObject::tr(
                          "TRYX accepted and activated the configuration, but device readback failed")
                    : QObject::tr(
                          "TRYX accepted and activated the configuration, but device readback failed: %1")
                          .arg(readback.error);
        }
        return false;
    }
    if (appliedState) {
        *appliedState = readback.state;
    }

    QStringList mismatches;
    if (activationRejected) {
        mismatches.append(QObject::tr("overlay activation"));
    }
    if (config.mediaPresent) {
        if (readback.state.screenMode != config.screenMode) {
            mismatches.append(QObject::tr("screen mode"));
        }
        if (readback.state.playMode != config.playMode) {
            mismatches.append(QObject::tr("play mode"));
        }
        if (readback.state.media != config.media) {
            mismatches.append(QObject::tr("media"));
        }
    }
    if (config.display.brightnessPresent &&
        readback.state.brightness != config.display.brightness) {
        mismatches.append(QObject::tr("brightness"));
    }
    if (config.display.backlightPresent &&
        readback.state.backlightEnabled != config.display.backlightEnabled) {
        mismatches.append(QObject::tr("display power"));
    }
    if (config.display.orientationPresent) {
        if (readback.state.mirrorMode != config.display.mirrorMode) {
            mismatches.append(QObject::tr("mirror"));
        }
        if (readback.state.waterfallMode != config.display.waterfallMode) {
            mismatches.append(QObject::tr("waterfall"));
        }
    }
    if (!mismatches.isEmpty()) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::VerificationFailed;
        }
        if (errorMessage) {
            *errorMessage =
                QObject::tr(
                    "TRYX device readback does not match the requested configuration: %1")
                    .arg(mismatches.join(QStringLiteral(", ")));
        }
        return false;
    }
    if (mutationDetails) {
        mutationDetails->outcome = MutationOutcome::Succeeded;
    }
    return true;
}

bool PaseConfigurationClient::configurePaseOverlay(const QString &devicePath,
                                                   const PaseOverlayConfig &overlay,
                                                   QString *errorMessage,
                                                   const OperationContext &context,
                                                   MutationDetails *mutationDetails) {
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("ActivatingMetricsLayout");
    }
    if (!productProfile_.overlayMetricsSupported) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("overlay metrics"));
        }
        return false;
    }
    const bool success = sendRunConfigTrigger(devicePath, errorMessage, context,
                                              &overlay, mutationDetails);
    channel_.closeDisplayActivationCycle();
    return success;
}

bool PaseConfigurationClient::sendPaseMetricBatch(
    const QString &devicePath, const PaseOverlayConfig &overlay,
    const QStringList &labels, const QStringList &values, const QStringList &units,
    QString *errorMessage, const OperationContext &context) {
    if (!productProfile_.overlayMetricsSupported) {
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("overlay metrics"));
        }
        return false;
    }
    if (!paseOverlayMetricSelectionIsValid(overlay, errorMessage)) {
        return false;
    }
    const QList<const PaseMetricDefinition *> leftSelected =
        paseSelectedMetrics(overlay.left);
    const QList<const PaseMetricDefinition *> rightSelected =
        overlay.dualMode ? paseSelectedMetrics(overlay.right)
                         : QList<const PaseMetricDefinition *>{};
    if (leftSelected.isEmpty() && rightSelected.isEmpty()) {
        return true;
    }

    panorama::wire::v1::Request request;
    auto *batch = request.mutable_metric_batch();
    const QDateTime now = QDateTime::currentDateTime();
    const auto appendArea = [batch, &labels, &values, &units, &now, &overlay](
                                const QList<const PaseMetricDefinition *> &selected,
                                quint32 idOffset) {
        for (const PaseMetricDefinition *definition : selected) {
            const quint32 groupId = definition->groupId + idOffset;
            const quint32 titleId = definition->titleId + idOffset;
            const quint32 valueId = definition->valueId + idOffset;
            const quint32 unitId =
                definition->unitId == 0 ? 0 : definition->unitId + idOffset;
            if (definition->dateTime) {
                addPaseLabelUpdate(
                    batch, groupId, titleId,
                    QLocale().toString(now.date(), QLocale::ShortFormat));
                addPaseLabelUpdate(
                    batch, groupId, valueId,
                    tryxFormatLocalTime(now.time(), overlay.timeFormat, QLocale()));
                continue;
            }
            const int valueIndex =
                labels.indexOf(QString::fromLatin1(definition->name));
            const bool valueAvailable = valueIndex >= 0 && valueIndex < values.size() &&
                                        !values.at(valueIndex).isEmpty();
            addPaseLabelUpdate(batch, groupId, valueId,
                               valueAvailable ? values.at(valueIndex)
                                              : QStringLiteral("--"));
            const bool temperatureMetric =
                definition->groupId == 100 || definition->groupId == 104;
            if (temperatureMetric) {
                const QString unit =
                    valueIndex >= 0 && valueIndex < units.size() &&
                            !units.at(valueIndex).isEmpty()
                        ? units.at(valueIndex)
                        : tryxTemperatureUnitSymbol(overlay.temperatureUnit);
                addPaseLabelUpdate(batch, groupId, unitId, unit);
            }
        }
    };
    appendArea(leftSelected, 0);
    if (overlay.dualMode) {
        appendArea(rightSelected, 100);
    }
    if (batch->label_groups().empty()) {
        return true;
    }
    return channel_.writeOnly(request, devicePath, context, errorMessage);
}

bool PaseConfigurationClient::setBrightness(const QString &devicePath, int brightness,
                                            QString *errorMessage,
                                            const OperationContext &context) {
    if (!productProfile_.displayConfigurationSupported) {
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("brightness control"));
        }
        return false;
    }
    PaseApplyConfig config;
    config.display.brightnessPresent = true;
    config.display.brightness = brightness;
    return applyPaseConfiguration(devicePath, config, errorMessage, context);
}

bool PaseConfigurationClient::sendUserConfigWithOutcome(
    const QString &devicePath, const panorama::wire::v1::UserConfiguration &userConfig,
    QString *errorMessage, const OperationContext &context,
    MutationDetails *mutationDetails) {
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("WritingConfig");
    }
    panorama::wire::v1::Request request;
    *request.mutable_user_configuration() = userConfig;
    panorama::wire::v1::Response response;
    PrinterTransactionChannel::TransactionOutcome outcome =
        PrinterTransactionChannel::TransactionOutcome::NotSent;
    if (channel_.execute(&request, panorama::wire::v1::Response::kAcknowledgement,
                         &response, devicePath, context, errorMessage, &outcome)) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
        }
        return true;
    }
    if (mutationDetails) {
        switch (outcome) {
        case PrinterTransactionChannel::TransactionOutcome::NotSent:
            mutationDetails->outcome = MutationOutcome::NotStarted;
            break;
        case PrinterTransactionChannel::TransactionOutcome::Cancelled:
            mutationDetails->outcome = MutationOutcome::Cancelled;
            break;
        case PrinterTransactionChannel::TransactionOutcome::Rejected:
            mutationDetails->outcome = MutationOutcome::Rejected;
            break;
        default:
            mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
            break;
        }
    }
    const bool fullySentButUnconfirmed =
        outcome == PrinterTransactionChannel::TransactionOutcome::SentOutcomeUnknown ||
        outcome ==
            PrinterTransactionChannel::TransactionOutcome::AcknowledgementTimeout ||
        outcome == PrinterTransactionChannel::TransactionOutcome::TransportFailure ||
        outcome == PrinterTransactionChannel::TransactionOutcome::InvalidResponse;
    if (!fullySentButUnconfirmed) {
        return false;
    }

    const QString transportError = errorMessage ? *errorMessage : QString();
    channel_.closeDevice();
    if (errorMessage) {
        *errorMessage =
            transportError.isEmpty()
                ? QObject::tr(
                      "The TRYX user configuration was fully sent, but its outcome was not confirmed. The configuration may already be stored; automatic rollback is disabled")
                : QObject::tr(
                      "The TRYX user configuration was fully sent, but its outcome was not confirmed: %1. The configuration may already be stored; automatic rollback is disabled")
                      .arg(transportError);
    }
    return false;
}

bool PaseConfigurationClient::activateAcceptedConfig(const QString &devicePath,
                                                     QString *errorMessage,
                                                     const OperationContext &context,
                                                     MutationDetails *mutationDetails,
                                                     const PaseOverlayConfig *overlay,
                                                     bool *activationRejected) {
    if (activationRejected) {
        *activationRejected = false;
    }
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("ActivatingConfig");
    }
    MutationDetails localMutationDetails;
    MutationDetails *activationDetails =
        mutationDetails ? mutationDetails : &localMutationDetails;
    QString activationError;
    if (sendRunConfigTrigger(devicePath, &activationError, context, overlay,
                             activationDetails)) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Succeeded;
        }
        return true;
    }
    if (activationDetails->outcome == MutationOutcome::Rejected) {
        if (activationRejected) {
            *activationRejected = true;
        }
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::VerificationFailed;
        }
        if (errorMessage) {
            *errorMessage =
                activationError.isEmpty()
                    ? QObject::tr(
                          "TRYX accepted the user configuration but rejected overlay activation")
                    : QObject::tr(
                          "TRYX accepted the user configuration but rejected overlay activation: %1")
                          .arg(activationError);
        }
        return false;
    }
    if (mutationDetails) {
        // UserConfiguration was already acknowledged before the layout was sent.
        // A cancellation or transport loss at this boundary cannot prove that
        // activation did not happen, so it must never be reported as a clean
        // cancellation that would be safe to replay automatically.
        mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
    }

    // UserConfiguration already received a successful acknowledgement. The
    // wire contract exposes no transaction or rollback primitive, so another write
    // would be an unsafe best-effort mutation, especially after a connection epoch
    // change. Close the uncertain session and report the partial boundary.
    channel_.closeDevice();
    if (errorMessage) {
        *errorMessage =
            activationError.isEmpty()
                ? QObject::tr(
                      "TRYX accepted the user configuration, but activation was not confirmed. The configuration may already be stored; automatic rollback is disabled")
                : QObject::tr(
                      "TRYX accepted the user configuration, but activation failed: %1. The configuration may already be stored; automatic rollback is disabled")
                      .arg(activationError);
    }
    return false;
}

bool PaseConfigurationClient::sendRunConfigTrigger(const QString &devicePath,
                                                   QString *errorMessage,
                                                   const OperationContext &context,
                                                   const PaseOverlayConfig *overlay,
                                                   MutationDetails *mutationDetails) {
    if (overlay && (!paseOverlayMetricSelectionIsValid(*overlay, errorMessage)
                    || !tryx::pase_overlay_config::paseBadgeChoicesAreValid(*overlay, productProfile_.productId, errorMessage))) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        return false;
    }
    panorama::wire::v1::Request request;
    if (overlay) {
        *request.mutable_overlay_layout() = buildPaseRunConfig(*overlay);
    } else {
        request.mutable_overlay_layout();
    }
    PrinterTransactionChannel::TransactionOutcome transactionOutcome =
        PrinterTransactionChannel::TransactionOutcome::NotSent;
    const bool success = channel_.writeTrackedOnly(&request, devicePath, context,
                                                   errorMessage, &transactionOutcome);
    if (mutationDetails) {
        if (success) {
            mutationDetails->outcome = MutationOutcome::Succeeded;
        } else {
            switch (transactionOutcome) {
            case PrinterTransactionChannel::TransactionOutcome::NotSent:
                mutationDetails->outcome = MutationOutcome::NotStarted;
                break;
            case PrinterTransactionChannel::TransactionOutcome::Cancelled:
                mutationDetails->outcome = MutationOutcome::Cancelled;
                break;
            case PrinterTransactionChannel::TransactionOutcome::Rejected:
                mutationDetails->outcome = MutationOutcome::Rejected;
                break;
            default:
                mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
                break;
            }
        }
    }
    return success;
}

PrinterProtocol::KeepaliveOutcome
PaseConfigurationClient::sendKeepalive(const QString &devicePath, QString *errorMessage,
                                       const OperationContext &context) {
    if (productProfile_.idleMode == PrinterIdleMode::TransferOnly) {
        if (operationIsCancelled(context)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX USB operation was cancelled because the device state changed");
            }
            return KeepaliveOutcome::FatalFailure;
        }
        if (errorMessage) {
            errorMessage->clear();
        }
        return KeepaliveOutcome::Sent;
    }
    return channel_.sendPeriodicFrame(
        PaseConfigurationClient::makeKeepaliveFrame(errorMessage), devicePath, context,
        errorMessage);
}

PrinterProtocol::KeepaliveOutcome PaseConfigurationClient::sendDisplayKeepalive(
    const QString &devicePath, QString *errorMessage, const OperationContext &context,
    const PaseOverlayConfig *overlay) {
    if (!productProfile_.overlayMetricsSupported) {
        if (errorMessage) {
            *errorMessage = unsupportedCapabilityError(
                productProfile_, QStringLiteral("display keepalive"));
        }
        return KeepaliveOutcome::FatalFailure;
    }
    if (overlay && (!paseOverlayMetricSelectionIsValid(*overlay, errorMessage)
                    || !tryx::pase_overlay_config::paseBadgeChoicesAreValid(*overlay, productProfile_.productId, errorMessage))) {
        return KeepaliveOutcome::FatalFailure;
    }
    panorama::wire::v1::Request request;
    // Match the observed peer: a periodic layout update is an untracked setter.
    // The peer waits only for USB OUT completion and handles an optional
    // acknowledgement in its shared asynchronous reader. Bootstrap and configuration
    // mutations use the tracked request path and still require their exact response.
    request.mutable_header();
    if (overlay) {
        *request.mutable_overlay_layout() = buildPaseRunConfig(*overlay);
    } else {
        request.mutable_overlay_layout();
    }
    return channel_.sendPeriodicRequest(request, devicePath, context, errorMessage);
}
